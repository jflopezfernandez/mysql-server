/* Copyright (c) 2010, 2018, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/filesort_utils.h"

#include <string.h>
#include <algorithm>
#include <cmath>

#include "add_with_saturate.h"
#include "my_dbug.h"
#include "my_io.h"
#include "my_pointer_arithmetic.h"
#include "sql/cmp_varlen_keys.h"
#include "sql/opt_costmodel.h"
#include "sql/sort_param.h"
#include "sql/sql_sort.h"
#include "sql/thr_malloc.h"

PSI_memory_key key_memory_Filesort_buffer_sort_keys;

namespace {
/**
  A local helper function. See comments for get_merge_buffers_cost().
 */
double get_merge_cost(ha_rows num_elements, ha_rows num_buffers, uint elem_size,
                      const Cost_model_table *cost_model) {
  const double io_ops = static_cast<double>(num_elements * elem_size) / IO_SIZE;
  const double io_cost = cost_model->io_block_read_cost(io_ops);
  const double cpu_cost =
      cost_model->key_compare_cost(num_elements * std::log2(num_buffers));
  return 2 * io_cost + cpu_cost;
}
}  // namespace

/**
  This is a simplified, and faster version of @see get_merge_many_buffs_cost().
  We calculate the cost of merging buffers, by simulating the actions
  of @see merge_many_buff. For explanations of formulas below,
  see comments for get_merge_buffers_cost().
  TODO: Use this function for Unique::get_use_cost().
*/
double get_merge_many_buffs_cost_fast(ha_rows num_rows,
                                      ha_rows num_keys_per_buffer,
                                      uint elem_size,
                                      const Cost_model_table *cost_model) {
  ha_rows num_buffers = num_rows / num_keys_per_buffer;
  ha_rows last_n_elems = num_rows % num_keys_per_buffer;
  double total_cost;

  // Calculate CPU cost of sorting buffers.
  total_cost =
      num_buffers * cost_model->key_compare_cost(
                        num_keys_per_buffer * log(1.0 + num_keys_per_buffer)) +
      cost_model->key_compare_cost(last_n_elems * log(1.0 + last_n_elems));

  // Simulate behavior of merge_many_buff().
  while (num_buffers >= MERGEBUFF2) {
    // Calculate # of calls to merge_buffers().
    const ha_rows loop_limit = num_buffers - MERGEBUFF * 3 / 2;
    const ha_rows num_merge_calls = 1 + loop_limit / MERGEBUFF;
    const ha_rows num_remaining_buffs =
        num_buffers - num_merge_calls * MERGEBUFF;

    // Cost of merge sort 'num_merge_calls'.
    total_cost +=
        num_merge_calls * get_merge_cost(num_keys_per_buffer * MERGEBUFF,
                                         MERGEBUFF, elem_size, cost_model);

    // # of records in remaining buffers.
    last_n_elems += num_remaining_buffs * num_keys_per_buffer;

    // Cost of merge sort of remaining buffers.
    total_cost += get_merge_cost(last_n_elems, 1 + num_remaining_buffs,
                                 elem_size, cost_model);

    num_buffers = num_merge_calls;
    num_keys_per_buffer *= MERGEBUFF;
  }

  // Simulate final merge_buff call.
  last_n_elems += num_keys_per_buffer * num_buffers;
  total_cost +=
      get_merge_cost(last_n_elems, 1 + num_buffers, elem_size, cost_model);
  return total_cost;
}

namespace {

/*
  An inline function which does memcmp().
  This one turns out to be pretty fast on all platforms, except sparc.
  See the accompanying unit tests, which measure various implementations.
 */
inline bool my_mem_compare(const uchar *s1, const uchar *s2, size_t len) {
  DBUG_ASSERT(len > 0);
  DBUG_ASSERT(s1 != NULL);
  DBUG_ASSERT(s2 != NULL);
  do {
    if (*s1++ != *s2++) return *--s1 < *--s2;
  } while (--len != 0);
  return false;
}

#define COMPARE(N) \
  if (s1[N] != s2[N]) return s1[N] < s2[N]

inline bool my_mem_compare_longkey(const uchar *s1, const uchar *s2,
                                   size_t len) {
  COMPARE(0);
  COMPARE(1);
  COMPARE(2);
  COMPARE(3);
  return memcmp(s1 + 4, s2 + 4, len - 4) < 0;
}

class Mem_compare {
 public:
  Mem_compare(size_t n) : m_size(n) {}
  bool operator()(const uchar *s1, const uchar *s2) const {
#ifdef __sun
    // The native memcmp is faster on SUN.
    return memcmp(s1, s2, m_size) < 0;
#else
    return my_mem_compare(s1, s2, m_size);
#endif
  }

 private:
  size_t m_size;
};

class Mem_compare_longkey {
 public:
  Mem_compare_longkey(size_t n) : m_size(n) {}
  bool operator()(const uchar *s1, const uchar *s2) const {
#ifdef __sun
    // The native memcmp is faster on SUN.
    return memcmp(s1, s2, m_size) < 0;
#else
    return my_mem_compare_longkey(s1, s2, m_size);
#endif
  }

 private:
  size_t m_size;
};

class Mem_compare_varlen_key {
 public:
  Mem_compare_varlen_key(const Bounds_checked_array<st_sort_field> sfa,
                         bool use_hash_arg)
      : sort_field_array(sfa.array(), sfa.size()), use_hash(use_hash_arg) {}

  bool operator()(const uchar *s1, const uchar *s2) const {
    return cmp_varlen_keys(sort_field_array, use_hash, s1, s2);
  }

 private:
  Bounds_checked_array<st_sort_field> sort_field_array;
  bool use_hash;
};

}  // namespace

void Filesort_buffer::sort_buffer(Sort_param *param, uint count) {
  const bool force_stable_sort = param->m_force_stable_sort;
  param->m_sort_algorithm = Sort_param::FILESORT_ALG_NONE;

  if (count <= 1) return;
  if (param->max_compare_length() == 0) return;

  if (param->using_varlen_keys()) {
    if (force_stable_sort) {
      param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_STABLE;
      std::stable_sort(
          begin(m_record_pointers), begin(m_record_pointers) + count,
          Mem_compare_varlen_key(param->local_sortorder, param->use_hash));
    } else {
      // TODO: Make more elaborate heuristics than just always picking
      // std::sort.
      param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_SORT;
      std::sort(
          begin(m_record_pointers), begin(m_record_pointers) + count,
          Mem_compare_varlen_key(param->local_sortorder, param->use_hash));
    }
    return;
  }

  /*
    std::stable_sort has some extra overhead in allocating the temp buffer,
    which takes some time. The cutover point where it starts to get faster
    than quicksort seems to be somewhere around 10 to 40 records.
    So we're a bit conservative, and stay with quicksort up to 100 records.
  */
  if (count <= 100 && !force_stable_sort) {
    if (param->max_compare_length() < 10) {
      param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_SORT;
      std::sort(begin(m_record_pointers), begin(m_record_pointers) + count,
                Mem_compare(param->max_compare_length()));
      return;
    }
    param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_SORT;
    std::sort(begin(m_record_pointers), begin(m_record_pointers) + count,
              Mem_compare_longkey(param->max_compare_length()));
    return;
  }

  /*
    stable_sort algorithm will be used. Either for performance reasons, or
    because force_stable_sort==true. In the latter case, we must exclude from
    the sort key the ref_length last bytes which were added in
    init_for_filesort(), so that those bytes do not cause a swapping of
    otherwise equivalent elements.
  */
  uint compare_len = param->max_compare_length();
  if (force_stable_sort && !param->using_addon_fields()) {
    DBUG_ASSERT(compare_len > param->ref_length && !param->using_varlen_keys());
    compare_len -= param->ref_length;  // ref was added last
  }
  param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_STABLE;
  // Heuristics here: avoid function overhead call for short keys.
  if (compare_len < 10)
    std::stable_sort(begin(m_record_pointers), begin(m_record_pointers) + count,
                     Mem_compare(compare_len));
  else
    std::stable_sort(begin(m_record_pointers), begin(m_record_pointers) + count,
                     Mem_compare_longkey(compare_len));
}

void Filesort_buffer::reset() {
  update_peak_memory_used();
  m_record_pointers.clear();
  if (m_blocks.size() >= 2) {
    // Free every block but the last.
    m_blocks.erase(m_blocks.begin(), m_blocks.end() - 1);
  }

  /*
    m_max_record_length can have changed since last time; if the remaining
    (largest) block is not large enough for a single row of the next size,
    then clear out that, too.
  */
  if (m_max_record_length > m_current_block_size) {
    free_sort_buffer();
  }

  if (m_blocks.empty()) {
    DBUG_ASSERT(m_next_rec_ptr == nullptr);
    DBUG_ASSERT(m_current_block_end == nullptr);
    DBUG_ASSERT(m_current_block_size == 0);
  } else {
    m_next_rec_ptr = m_blocks[0].get();
    DBUG_ASSERT(m_current_block_end == m_next_rec_ptr + m_current_block_size);
  }
  m_space_used_other_blocks = 0;
}

bool Filesort_buffer::preallocate_records(size_t num_records) {
  reset();

  const size_t bytes_needed = num_records * m_max_record_length;
  if (bytes_needed + num_records * sizeof(m_record_pointers[0]) >
      m_max_size_in_bytes) {
    return true;
  }

  /*
    If the remaining block can't hold what we need, then it's of no
    use to us (it doesn't save us any allocations), so get rid of it
    and allocate one that's exactly the right size.
  */
  if (m_next_rec_ptr + bytes_needed > m_current_block_end) {
    free_sort_buffer();
    if (allocate_sized_block(bytes_needed)) {
      return true;
    }
    m_record_pointers.reserve(num_records);
  }
  while (m_record_pointers.size() < num_records) {
    uchar *ptr = get_next_record_pointer();
    (void)ptr;
    DBUG_ASSERT(ptr != nullptr);
  }
  return false;
}

bool Filesort_buffer::allocate_block(size_t num_rows) {
  DBUG_EXECUTE_IF("alloc_sort_buffer_fail",
                  DBUG_SET("+d,simulate_out_of_memory"););

  const size_t bytes_needed = num_rows * m_max_record_length;

  size_t next_block_size;
  if (m_current_block_size == 0) {
    // First block.
    next_block_size = MIN_SORT_MEMORY;
  } else {
    next_block_size = m_current_block_size + m_current_block_size / 2;
  }

  // Figure out how much space we've used, to see how much is left (if
  // anything).
  size_t space_used = m_current_block_size + m_space_used_other_blocks;
  space_used += m_record_pointers.capacity() * sizeof(m_record_pointers[0]);

  size_t space_left;
  if (space_used > m_max_size_in_bytes)
    space_left = 0;
  else
    space_left = m_max_size_in_bytes - space_used;

  /*
    Adjust space_left to take into account that filling this new buffer
    with records would necessarily also add pointers to m_record_pointers.
    Note that we know how much space record_pointers currently is using,
    but not how much it could potentially be using in the future as we add
    records; we take a best-case estimate based on maximum-size records.
    It's also impossible to say how capacity() will change since this
    is an implementation detail, so we don't take that into account.
    This means that, for smaller records, we could go above the maximum
    permitted total memory usage.
  */
  size_t min_num_rows_capacity =
      m_record_pointers.size() +
      space_left /
          AddWithSaturate(m_max_record_length, sizeof(m_record_pointers[0]));
  if (min_num_rows_capacity > m_record_pointers.capacity()) {
    space_left -= (min_num_rows_capacity - m_record_pointers.capacity()) *
                  sizeof(m_record_pointers[0]);
  }

  next_block_size =
      std::min(std::max(next_block_size, bytes_needed), space_left);
  if (next_block_size < bytes_needed) {
    /*
      If we're really out of space, but have at least 32 kB unused in
      m_record_pointers, try to reclaim some space and try again. This should
      only be needed in some very rare cases where we first sort a lot of very
      short rows (yielding a huge amount of record pointers) and then need to
      sort huge rows that wouldn't fit in the buffer otherwise -- in other
      words, nearly never.
    */
    size_t excess_bytes =
        (m_record_pointers.capacity() - m_record_pointers.size()) *
        sizeof(m_record_pointers[0]);
    if (excess_bytes >= 32768) {
      size_t old_capacity = m_record_pointers.capacity();
      m_record_pointers.shrink_to_fit();
      if (m_record_pointers.capacity() < old_capacity) {
        return allocate_block(num_rows);
      }
    }

    // We're full.
    return true;
  }

  return allocate_sized_block(next_block_size);
}

bool Filesort_buffer::allocate_sized_block(size_t block_size) {
  unique_ptr_my_free<uchar[]> new_block((uchar *)my_malloc(
      key_memory_Filesort_buffer_sort_keys, block_size, MYF(0)));
  if (new_block == nullptr) {
    return true;
  }

  m_space_used_other_blocks += m_current_block_size;
  m_current_block_size = block_size;
  m_next_rec_ptr = new_block.get();
  m_current_block_end = new_block.get() + m_current_block_size;
  m_blocks.push_back(std::move(new_block));

  return false;
}

void Filesort_buffer::free_sort_buffer() {
  update_peak_memory_used();

  // std::vector::clear() does not necessarily free all the memory,
  // but moving or swapping in an empty vector typically does (and we
  // rely on this, even though we really shouldn't). This shouldn't have
  // been a problem since they will be cleared in the destructor, but
  // there are _many_ places scattered around the code that construct TABLE
  // objects (which indirectly contain Filesort_buffer objects) and never
  // destroy them properly. (You can find lots of them easily by adding an
  // std::unique_ptr<int> to Filesort_buffer and giving it a value in the
  // constructor; it will leak all over the place.) We should fix that,
  // but for the time being, we have this workaround instead.
  m_record_pointers = std::vector<uchar *>();
  m_blocks = std::vector<unique_ptr_my_free<uchar[]>>();

  m_space_used_other_blocks = 0;
  m_next_rec_ptr = nullptr;
  m_current_block_end = nullptr;
  m_current_block_size = 0;
}

Bounds_checked_array<uchar> Filesort_buffer::get_contiguous_buffer() {
  if (m_current_block_size != m_max_size_in_bytes) {
    free_sort_buffer();

    if (allocate_sized_block(m_max_size_in_bytes)) {
      return Bounds_checked_array<uchar>(nullptr, 0);
    }
  }
  return Bounds_checked_array<uchar>(m_blocks.back().get(),
                                     m_max_size_in_bytes);
}

void Filesort_buffer::update_peak_memory_used() const {
  m_peak_memory_used =
      std::max(m_peak_memory_used,
               m_record_pointers.capacity() * sizeof(m_record_pointers[0]) +
                   m_current_block_size + m_space_used_other_blocks);
}
