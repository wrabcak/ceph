// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Bitmap based in-memory allocator.
 * Author: Ramesh Chander, Ramesh.Chander@sandisk.com
 *
 * TBD list:
 *  1. Make commiting and un commiting lists concurrent.
 */

#include "BitAllocator.h"

#include "BitMapAllocator.h"
#include "bluestore_types.h"
#include "BlueStore.h"

#define dout_subsys ceph_subsys_bluestore
#undef dout_prefix
#define dout_prefix *_dout << "bitmapalloc:"


BitMapAllocator::BitMapAllocator(int64_t device_size, int64_t block_size)
  : m_num_uncommitted(0),
    m_num_committing(0)
{
  int64_t zone_size_blks = 1024; // Change it later

  m_block_size = block_size;
  m_bit_alloc = new BitAllocator(device_size / block_size,
        zone_size_blks, CONCURRENT, true);
  assert(m_bit_alloc);
  if (!m_bit_alloc) {
    dout(10) << __func__ << "Unable to intialize Bit Allocator" << dendl;
  }
  dout(10) << __func__ << " instance " << (uint64_t) this
           << " size 0x" << std::hex << device_size << std::dec
           << dendl;
}

BitMapAllocator::~BitMapAllocator()
{
  delete m_bit_alloc;
}

void BitMapAllocator::insert_free(uint64_t off, uint64_t len)
{
  dout(20) << __func__ << " instance " << (uint64_t) this
           << " off 0x" << std::hex << off
           << " len 0x" << len << std::dec
           << dendl;

  assert(!(off % m_block_size));
  assert(!(len % m_block_size));

  m_bit_alloc->free_blocks(off / m_block_size,
             len / m_block_size);
}

int BitMapAllocator::reserve(uint64_t need)
{
  int nblks = need / m_block_size; // apply floor
  assert(!(need % m_block_size));
  dout(10) << __func__ << " instance " << (uint64_t) this
           << " num_used " << m_bit_alloc->get_used_blocks()
           << " total " << m_bit_alloc->size()
           << dendl;

  if (!m_bit_alloc->reserve_blocks(nblks)) {
    return -ENOSPC;
  }
  return 0;
}

void BitMapAllocator::unreserve(uint64_t unused)
{
  int nblks = unused / m_block_size;
  assert(!(unused % m_block_size));

  dout(10) << __func__ << " instance " << (uint64_t) this
           << " unused " << nblks
           << " num used " << m_bit_alloc->get_used_blocks()
           << " total " << m_bit_alloc->size()
           << dendl;

  m_bit_alloc->unreserve_blocks(nblks);
}

int BitMapAllocator::allocate(
  uint64_t want_size, uint64_t alloc_unit, int64_t hint,
  uint64_t *offset, uint32_t *length)
{

  assert(!(alloc_unit % m_block_size));
  int64_t nblks = (want_size + m_block_size - 1) / m_block_size;

  assert(alloc_unit);

  int64_t start_blk = 0;
  int64_t count = 0;

  dout(10) << __func__ << " instance " << (uint64_t) this
           << " want_size 0x" << std::hex << want_size
           << " alloc_unit 0x" << alloc_unit
           << " hint 0x" << hint << std::dec
           << dendl;

  *offset = 0;
  *length = 0;

  count = m_bit_alloc->alloc_blocks_res(nblks, &start_blk);
  if (count == 0) {
    return -ENOSPC;
  }
  *offset = start_blk * m_block_size;
  *length = count * m_block_size;

  dout(20) << __func__ <<" instance "<< (uint64_t) this
           << " offset 0x" << std::hex << *offset
           << " length 0x" << *length << std::dec
           << dendl;

  return 0;
}

int BitMapAllocator::release(
  uint64_t offset, uint64_t length)
{
  std::lock_guard<std::mutex> l(m_lock);
  dout(10) << __func__ << " 0x"
           << std::hex << offset << "~" << length << std::dec
           << dendl;
  m_uncommitted.insert(offset, length);
  m_num_uncommitted += length;
  return 0;
}

uint64_t BitMapAllocator::get_free()
{
  return ((
    m_bit_alloc->size() - m_bit_alloc->get_used_blocks()) *
    m_block_size);
}

void BitMapAllocator::dump(ostream& out)
{
  std::lock_guard<std::mutex> l(m_lock);
  dout(30) << __func__ << " instance " << (uint64_t) this
           << " committing: " << m_committing.num_intervals() << " extents"
           << dendl;

  for (auto p = m_committing.begin();
    p != m_committing.end(); ++p) {
    dout(30) << __func__ << " instance " << (uint64_t) this
             << " 0x" << std::hex << p.get_start()
             << "~" << p.get_len() << std::dec
             << dendl;
  }
  dout(30) << __func__ << " instance " << (uint64_t) this
           << " uncommitted: " << m_uncommitted.num_intervals() << " extents"
           << dendl;

  for (auto p = m_uncommitted.begin();
    p != m_uncommitted.end(); ++p) {
    dout(30) << __func__ << " 0x" << std::hex << p.get_start()
             << "~" << p.get_len() << std::dec
             << dendl;
  }
}

void BitMapAllocator::init_add_free(uint64_t offset, uint64_t length)
{
  dout(10) << __func__ << " instance " << (uint64_t) this
           << " offset 0x" << std::hex << offset
           << " length 0x" << length << std::dec
           << dendl;
  uint64_t size = m_bit_alloc->size() * m_block_size;

  uint64_t offset_adj = ROUND_UP_TO(offset, m_block_size);
  uint64_t length_adj = ((length - (offset_adj - offset)) /
                         m_block_size) * m_block_size;

  if ((offset_adj + length_adj) > size) {
    assert(((offset_adj + length_adj) - m_block_size) < size);
    length_adj = size - offset_adj;
  }

  insert_free(offset_adj, length_adj);
}

void BitMapAllocator::init_rm_free(uint64_t offset, uint64_t length)
{
  dout(10) << __func__ << " instance " << (uint64_t) this
           << " offset 0x" << std::hex << offset
           << " length 0x" << length << std::dec
           << dendl;

  assert(!(offset % m_block_size));
  assert(!(length % m_block_size));

  int64_t first_blk = offset / m_block_size;
  int64_t count = length / m_block_size;

  m_bit_alloc->set_blocks_used(first_blk, count);
}


void BitMapAllocator::shutdown()
{
  dout(10) << __func__ << " instance " << (uint64_t) this << dendl;
  m_bit_alloc->shutdown();
}

void BitMapAllocator::commit_start()
{
  std::lock_guard<std::mutex> l(m_lock);

  dout(10) << __func__ << " instance " << (uint64_t) this
           << " releasing " << m_num_uncommitted
           << " in extents " << m_uncommitted.num_intervals()
           << dendl;
  assert(m_committing.empty());
  m_committing.swap(m_uncommitted);
  m_num_committing = m_num_uncommitted;
  m_num_uncommitted = 0;
}

void BitMapAllocator::commit_finish()
{
  std::lock_guard<std::mutex> l(m_lock);
  dout(10) << __func__ << " instance " << (uint64_t) this
           << " released " << m_num_committing
           << " in extents " << m_committing.num_intervals()
           << dendl;
  for (auto p = m_committing.begin();
    p != m_committing.end();
    ++p) {
    insert_free(p.get_start(), p.get_len());
  }
  m_committing.clear();
  m_num_committing = 0;
}
