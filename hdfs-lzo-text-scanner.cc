// Copyright (c) 2012 Cloudera, Inc. All rights reserved.
// This file is based on code from the lzop program which is:
//   Copyright (C) 1996-2010 Markus Franz Xaver Johannes Oberhumer
//   All Rights Reserved.
//
//   lzop and the LZO library are free software; you can redistribute them
//   and/or modify them under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.
//   If not, write to the Free Software Foundation, Inc.,
//   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.


#include <hdfs.h>
#include <dlfcn.h>
#include <boost/algorithm/string.hpp>
#include "hdfs-lzo-text-scanner.h"
#include "exec/hdfs-scan-node.h"
#include "exec/scanner-context.inline.h"
#include "runtime/runtime-state.h"
#include "runtime/hdfs-fs-cache.h"
#include "util/debug-util.h"
#include "util/hdfs-util.h"

#include "gen-cpp/Descriptors_types.h"

using namespace boost;
using namespace boost::algorithm;
using namespace impala;
using namespace std;

DEFINE_bool(disable_lzo_checksums, true,
    "Disable internal checksum checking for Lzo compressed files, defaults true");

// Suffix for index file: hdfs-filename.index
const string HdfsLzoTextScanner::INDEX_SUFFIX = ".index";

// The magic byte sequence at the beginning of an LZOP file.
static const uint8_t LZOP_MAGIC[9] =
    { 0x89, 0x4c, 0x5a, 0x4f, 0x00, 0x0d, 0x0a, 0x1a, 0x0a };

extern "C" HdfsLzoTextScanner* CreateLzoTextScanner(
    HdfsScanNode* scan_node, RuntimeState* state) {
  return new HdfsLzoTextScanner(scan_node, state);
}

extern "C" Status IssueInitialRanges(HdfsScanNode* scan_node,
    const vector<HdfsFileDesc*>& files) {
  return HdfsLzoTextScanner::IssueInitialRanges(scan_node, files);
}

namespace impala {

HdfsLzoTextScanner::HdfsLzoTextScanner(HdfsScanNode* scan_node, RuntimeState* state)
    : HdfsTextScanner(scan_node, state),
      block_buffer_pool_(new MemPool(state->mem_limits())),
      block_buffer_len_(0),
      bytes_remaining_(0),
      past_eosr_(false),
      eos_read_(false),
      only_parsing_header_(false),
      disable_checksum_(FLAGS_disable_lzo_checksums) {
  decompress_timer_ = ADD_TIMER(scan_node->runtime_profile(), "DecompressionTime");
}

HdfsLzoTextScanner::~HdfsLzoTextScanner() {
  COUNTER_UPDATE(scan_node_->memory_used_counter(),
    block_buffer_pool_->peak_allocated_bytes());
}

Status HdfsLzoTextScanner::Close() {
  AttachPool(block_buffer_pool_.get());
  AddFinalRowBatch();
  context_->Close();
  if (!only_parsing_header_) {
    scan_node_->RangeComplete(THdfsFileFormat::LZO_TEXT, THdfsCompression::NONE);
  }
  scan_node_->ReleaseCodegenFn(THdfsFileFormat::LZO_TEXT, codegen_fn_);
  codegen_fn_ = NULL;
  return Status::OK;
}

Status HdfsLzoTextScanner::ProcessSplit() {
  past_eosr_ = false;
  header_ = reinterpret_cast<LzoFileHeader*>(
      scan_node_->GetFileMetadata(stream_->filename()));
  if (header_ == NULL) {
    // This is the initial scan range just to parse the header
    only_parsing_header_ = true;
    header_ = state_->obj_pool()->Add(new LzoFileHeader());
    // Parse the header and read the index file.
    RETURN_IF_ERROR(ReadHeader());
    RETURN_IF_ERROR(ReadIndexFile());

    // Header is parsed, set the metadata in the scan node.
    scan_node_->SetFileMetadata(stream_->filename(), header_);
    return IssueFileRanges(stream_->filename());
  }
  only_parsing_header_ = false;

  if (stream_->scan_range()->offset() == 0) {
    Status status;
    stream_->SkipBytes(header_->header_size_, &status);
  } else {
    DCHECK(!header_->offsets.empty());
    RETURN_IF_ERROR(FindFirstBlock());
  }

  RETURN_IF_ERROR(HdfsTextScanner::ProcessSplit());
  return Status::OK;
}

Status HdfsLzoTextScanner::IssueInitialRanges(HdfsScanNode* scan_node,
    const vector<HdfsFileDesc*>& files) {
  vector<DiskIoMgr::ScanRange*> header_ranges;
  // Issue just the header range for each file.  When the header is complete,
  // we'll issue the ranges for that file.  Read the minimum header size plus
  // up to 255 bytes of optional file name.
  for (int i = 0; i < files.size(); ++i) {
    // These files should be filtered by the planner.
    DCHECK(!ends_with(files[i]->filename, INDEX_SUFFIX));

    ScanRangeMetadata* metadata =
        reinterpret_cast<ScanRangeMetadata*>(files[i]->splits[0]->meta_data());
    DiskIoMgr::ScanRange* header_range = scan_node->AllocateScanRange(
        files[i]->filename.c_str(), HEADER_SIZE, 0, metadata->partition_id, -1);
    header_ranges.push_back(header_range);
  }
  RETURN_IF_ERROR(scan_node->AddDiskIoRanges(header_ranges));
  return Status::OK;
}

Status HdfsLzoTextScanner::IssueFileRanges(const char* filename) {
  HdfsFileDesc* file_desc = scan_node_->GetFileDesc(filename);
  if (header_->offsets.empty()) {
    // If offsets is empty then there was on index file.  The file cannot be split.
    // If this contains the range starting at offset 0 generate a scan for whole file.
    const vector<DiskIoMgr::ScanRange*>& splits = file_desc->splits;
    vector<DiskIoMgr::ScanRange*> ranges;
    for (int j = 0; j < splits.size(); ++j) {
      if (splits[j]->offset() != 0) {
        // Mark the other initial splits complete
        scan_node_->RangeComplete(THdfsFileFormat::LZO_TEXT, THdfsCompression::NONE);
        continue;
      }
      ScanRangeMetadata* metadata =
          reinterpret_cast<ScanRangeMetadata*>(file_desc->splits[0]->meta_data());
      DiskIoMgr::ScanRange* range = scan_node_->AllocateScanRange(
          filename, file_desc->file_length, 0, metadata->partition_id, -1);
      ranges.push_back(range);
    }
    scan_node_->AddDiskIoRanges(ranges);
  } else {
    scan_node_->AddDiskIoRanges(file_desc);
  }
  return Status::OK;
}

Status HdfsLzoTextScanner::ReadIndexFile() {
  string index_filename(stream_->filename());
  index_filename.append(INDEX_SUFFIX);

  hdfsFS connection = scan_node_->hdfs_connection();

  // If there is no index file we can read the file by starting at the beginning
  // and reading through to the end.
  if (hdfsExists(connection, index_filename.c_str()) != 0) {
    LOG(WARNING) << "No index file for: " << stream_->filename()
                 << ". Split scans are not possible.";
    return Status::OK;
  }

  hdfsFile index_file = hdfsOpenFile(connection,
       index_filename.c_str(), O_RDONLY, 0, 0, 0);

  if (index_file == NULL) {
    stringstream ss;
    ss << AppendHdfsErrorMessage("Error while opening index file: ", index_filename);
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  // TODO: This should go through the I/O manager.
  int read_size = 10 * 1024;
  uint8_t buffer[read_size];
  int num_read;

  while ((num_read = hdfsRead(connection, index_file, buffer, read_size)) > 0) {
    DCHECK_EQ(num_read % sizeof (int64_t), 0);
    for (uint8_t* bp = buffer; bp < buffer + num_read; bp += sizeof(int64_t)) {
      int64_t offset = ReadWriteUtil::GetInt<uint64_t>(bp);
      header_->offsets.push_back(offset);
    }
  }

  int close_stat  = hdfsCloseFile(connection, index_file);

  if (num_read == -1) {
    stringstream ss;
    ss << AppendHdfsErrorMessage("Error while reading index file: ", index_filename);
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  if (close_stat == -1) {
    stringstream ss;
    ss << AppendHdfsErrorMessage("Error while closing index file: ", index_filename);
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  return Status::OK;
}

Status HdfsLzoTextScanner::FindFirstBlock() {
  int64_t offset = stream_->file_offset();

  // Find the first block at or after the current file offset.  That way the
  // scan will start, or restart, on a block boundary.
  vector<int64_t>::iterator pos =
      upper_bound(header_->offsets.begin(), header_->offsets.end(), offset);

  if (pos == header_->offsets.end()) {
    stringstream ss;
    ss << "No block index for " << stream_->filename() << " after offset: " << offset;
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  VLOG_ROW << "First Block: " << stream_->filename()
           << " for " << offset << " @" << *pos;
  Status status;
  stream_->SkipBytes(*pos - offset, &status);
  return status;
}

Status HdfsLzoTextScanner::ReadData() {
  do {
    Status status = ReadAndDecompressData();

    if (status.ok() || state_->abort_on_error()) return status;

    // On error try to skip forward to the next block.
    status = FindFirstBlock();
    if (!status.ok()) {
      if (state_->abort_on_error()) return status;

      // Just force to end of file, we cannot do more recovery if we can't find
      // the next block
      eos_read_ = true;
      bytes_remaining_ = 0;
      return Status::OK;
    }
  } while (!stream_->eosr());

  // Reset the scanner state.
  HdfsTextScanner::ResetScanner();
  return Status::OK;
}

Status HdfsLzoTextScanner::FillByteBuffer(bool* eosr, int num_bytes) {
  *eosr = false;
  byte_buffer_read_size_ = 0;

  if (stream_->eof()) {
    *eosr = true;
    return Status::OK;
  }

  if (stream_->eosr()) {
    // Set the read size to be the biggest a block could be. This needs
    // to be done here because the text scanner will set it to something
    // smaller during initialization.
    stream_->set_read_past_buffer_size(MAX_BLOCK_COMPRESSED_SIZE);
    past_eosr_ = true;
    VLOG_ROW << "Reading past eosr: " << stream_->filename()
             << " @" << stream_->file_offset();
  }

  // Figure out if we have enough data and read more if necessary.
  if ((num_bytes == 0 && bytes_remaining_ == 0) || num_bytes > bytes_remaining_) {
    // Read and decompress the next block.
    RETURN_IF_ERROR(ReadData());
  }

  if (bytes_remaining_ != 0) {
    if (bytes_remaining_ >= num_bytes) {
      // We have enough bytes left to fill the request.
      byte_buffer_ptr_ = reinterpret_cast<char*>(block_buffer_ptr_);
      if (num_bytes == 0) {
         byte_buffer_read_size_ = bytes_remaining_;
      } else {
         byte_buffer_read_size_ = num_bytes;
      }
    } else {
      byte_buffer_ptr_ = reinterpret_cast<char*>(block_buffer_ptr_);
      byte_buffer_read_size_ = num_bytes;
    }
    // We assume a block is larger than the largest request.
    if (!eos_read_ && num_bytes > bytes_remaining_) {
      // Text only reads everything or 1024 so we do not need to handle this case.
      if (state_->LogHasSpace()) {
        state_->LogError("Unexpected read size in LZO decompressor");
      }
      DCHECK_LE(num_bytes, bytes_remaining_);
      return Status("Unexpected read size in LZO decompressor");
    }
  }

  byte_buffer_end_ = byte_buffer_ptr_ + byte_buffer_read_size_;
  if (bytes_remaining_ != 0) {
    bytes_remaining_ -= byte_buffer_read_size_;
    block_buffer_ptr_ += byte_buffer_read_size_;
  }

  *eosr = past_eosr_ || (eos_read_ && bytes_remaining_ == 0);

  if (VLOG_ROW_IS_ON && *eosr) {
    VLOG_ROW << "Returning eosr for: " << stream_->filename()
             << " @" << stream_->file_offset();
  }
  return Status::OK;
}

Status HdfsLzoTextScanner::Checksum(LzoChecksum type, const string& source,
    int expected_checksum, uint8_t* buffer, int length) {

  if (disable_checksum_) return Status::OK;

  // Do the checksum if requested.
  int32_t calculated_checksum;
  switch (type) {
    case CHECK_NONE:
      return Status::OK;

    case CHECK_CRC32:
      calculated_checksum = lzo_crc32(CRC32_INIT_VALUE, buffer, length);
      break;

    case CHECK_ADLER:
      calculated_checksum = lzo_adler32(ADLER32_INIT_VALUE, buffer, length);
      break;

    default:
      DCHECK(false);
  }

  if (calculated_checksum != expected_checksum) {
    stringstream ss;
    ss << "Checksum of " << source << " block failed on file: " << stream_->filename()
       << " at offset: " << stream_->file_offset() - length
       << " expected: " << expected_checksum << " got: " << calculated_checksum;
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }
  return Status::OK;
}

Status HdfsLzoTextScanner::ReadHeader() {
  uint8_t* magic;
  int num_read;
  bool eos;
  Status status;
  // Read the header in. HEADER_SIZE over estimates the maximum header.
  stream_->GetBytes(HEADER_SIZE, &magic, &num_read, &eos, &status);
  RETURN_IF_ERROR(status);

  if (num_read < MIN_HEADER_SIZE) {
    stringstream ss;
    ss << "Read only " << num_read << " bytes from " << stream_->filename();
    return Status(ss.str());
  }

  if (memcmp(magic, LZOP_MAGIC, sizeof(LZOP_MAGIC))) {
    stringstream ss;
    ss << "Invalid LZOP_MAGIC: '"
       << ReadWriteUtil::HexDump(magic, sizeof(LZOP_MAGIC)) << "'" << endl;
    status.AddErrorMsg(ss.str());
  } 

  uint8_t* header = magic + sizeof(LZOP_MAGIC);
  uint8_t* h_ptr = header;

  int version = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (version > LZOP_VERSION) {
    stringstream ss;
    ss << "Compressed with later version of lzop: " << version
       << " must be less than: " << LZOP_VERSION;
    status.AddErrorMsg(ss.str());
  }
  h_ptr += sizeof(int16_t);

  int libversion = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (libversion < MIN_LZO_VERSION) {
    stringstream ss;
    ss << "Compressed with incompatible lzo version: " << version
       << "must be at least: " << MIN_ZOP_VERSION;
    status.AddErrorMsg(ss.str());
  }
  h_ptr += sizeof(int16_t);

  // The version of LZOP needed to interpret this file.
  int neededversion = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (neededversion > LZOP_VERSION) {
    stringstream ss;
    ss << "Compressed with imp incompatible lzo version: " << neededversion
       << "must be at no more than: " << LZOP_VERSION;
    status.AddErrorMsg(ss.str());
  }
  h_ptr += sizeof(int16_t);

  uint8_t method = *h_ptr++;
  if (method < 1 || method > 3) {
    stringstream ss;
    ss << "Invalid compression method: " << method;
    status.AddErrorMsg(ss.str());
  }
  uint8_t level = *h_ptr++;

  int flags = ReadWriteUtil::GetInt<uint32_t>(h_ptr);
  LzoChecksum header_checksum = (flags & F_H_CRC32) ? CHECK_CRC32 : CHECK_ADLER;
  header_->output_checksum_type_ = (flags & F_CRC32_D) ? CHECK_CRC32 :
      (flags & F_ADLER32_D) ? CHECK_ADLER : CHECK_NONE;
  header_->input_checksum_type_ = (flags & F_CRC32_C) ? CHECK_CRC32 :
      (flags & F_ADLER32_C) ? CHECK_ADLER : CHECK_NONE;
  
  if (flags & (F_RESERVED | F_MULTIPART | F_H_FILTER)) {
    stringstream ss;
    ss << "Unsupported flags: " << flags;
    status.AddErrorMsg(ss.str());
  }
  h_ptr += sizeof(int32_t);

  // skip mode and time fields
  h_ptr += 3 * sizeof(int32_t);

  // Skip filename.
  h_ptr += *h_ptr + 1;

  // The header always has a checksum.
  int32_t expected_checksum = ReadWriteUtil::GetInt<uint32_t>(h_ptr);
  int32_t computed_checksum;
  if (header_checksum == CHECK_CRC32) {
    computed_checksum = CRC32_INIT_VALUE;
    computed_checksum = lzo_crc32(computed_checksum, header, h_ptr - header);
  } else {
    computed_checksum = ADLER32_INIT_VALUE;
    computed_checksum = lzo_adler32(computed_checksum, header, h_ptr - header);
  }

  if (computed_checksum != expected_checksum) {
    stringstream ss;
    ss << "Invalid header checksum: " << computed_checksum
       << " expected: " << expected_checksum;
    status.AddErrorMsg(ss.str());
  }
  h_ptr += sizeof(int32_t);

  // Skip the extra field if any.
  if (flags & F_H_EXTRA_FIELD) {
    int32_t len;
    Status status;
    stream_->ReadInt(&len, &status);
    RETURN_IF_ERROR(status);
    // Add the size of the len and the checksum and the len to the total h_ptr size.
    h_ptr += (2 * sizeof(int32_t)) + len;
  }

  VLOG_FILE << "Reading: " << stream_->filename() << " Header: version: " << version 
            << "(" << libversion << "/" << neededversion << ")"
            << " method: " << (int)method << "@" << (int)level
            << " flags: " << flags;
  if (!status.ok()) {
    stringstream ss;
    ss << "Invalid header information: " << stream_->filename();
    status.AddErrorMsg(ss.str());
    return status;
  }

  header_->header_size_ = h_ptr - magic;

  return Status::OK;
}

Status HdfsLzoTextScanner::ReadAndDecompressData() {
  bytes_remaining_ = 0;
  Status status;
  
  // Read the uncompressed
  int32_t uncompressed_len = 0, compressed_len = 0;
  stream_->ReadInt(&uncompressed_len, &status);
  RETURN_IF_ERROR(status);
  if (uncompressed_len == 0) {
    DCHECK(stream_->eosr());
    eos_read_ = true;
    return Status::OK;
  }

  // Read the compressed len
  stream_->ReadInt(&compressed_len, &status);
  RETURN_IF_ERROR(status);

  if (compressed_len > LZO_MAX_BLOCK_SIZE) {
    stringstream ss;
    ss << "Blocksize: " << compressed_len << " is greater than LZO_MAX_BLOCK_SIZE: "
       << LZO_MAX_BLOCK_SIZE;
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  int out_checksum;
  // The checksum of the uncompressed data.
  if (header_->output_checksum_type_ != CHECK_NONE) {
    stream_->ReadInt(&out_checksum, &status);
    RETURN_IF_ERROR(status);
  }
  
  int in_checksum = 0;
  if (compressed_len < uncompressed_len && header_->input_checksum_type_ != CHECK_NONE) {
    stream_->ReadInt(&in_checksum, &status);
    RETURN_IF_ERROR(status);
  } else {
    // If the compressed data size is equal to the uncompressed data size, then
    // the uncompressed data is stored and there is no compressed checksum.
    in_checksum = out_checksum;
  }

  // Read in the compressed data
  uint8_t* compressed_data;
  int bytes_read;
  stream_->GetBytes(compressed_len, &compressed_data, &bytes_read, &eos_read_, &status);
  DCHECK_EQ(compressed_len, bytes_read);
  RETURN_IF_ERROR(status);

  // Checksum the data.
  RETURN_IF_ERROR(Checksum(header_->input_checksum_type_,
      "compressed", in_checksum, compressed_data, compressed_len));

  // If the compressed length is the same as the uncompressed length, it means the data
  // was not compressed and we are done.
  if (compressed_len == uncompressed_len) {
    block_buffer_ptr_ = compressed_data;
    bytes_remaining_ = uncompressed_len;
    return Status::OK;
  }

  if (!stream_->compact_data()) {
    AttachPool(block_buffer_pool_.get());
    block_buffer_len_ = 0;
  }

  if (uncompressed_len > block_buffer_len_) {
    block_buffer_ = block_buffer_pool_->Allocate(uncompressed_len);
    block_buffer_len_ = uncompressed_len;
  }
  block_buffer_ptr_ = block_buffer_;
  bytes_remaining_ = uncompressed_len;

  // Decompress the data.  lzop always uses lzo1x.
  SCOPED_TIMER(decompress_timer_);
  int ret = lzo1x_decompress_safe(compressed_data, compressed_len,
      block_buffer_, reinterpret_cast<lzo_uint*>(&uncompressed_len), NULL);

  if (ret != LZO_E_OK || bytes_remaining_ != uncompressed_len) {
    stringstream ss;
    ss << "Decompression failed on file: " << stream_->filename()
       << " at offset: " << stream_->file_offset() << " returned: " << ret
       << " output size: " << compressed_len << "expected: " << block_buffer_len_;
      state_->LogError(ss.str());
    if (state_->LogHasSpace()) state_->LogError(ss.str());
    return Status(ss.str());
  }

  // Do the checksum if requested.
  RETURN_IF_ERROR(Checksum(header_->output_checksum_type_,
     "decompressed", out_checksum, block_buffer_, uncompressed_len));

  // Return end of scan range even if there are bytes in the disk buffer.
  // We fetched the next disk buffer past EOSR to complete the read of this compressed
  // block.  When the scanner finishes with the data we return here it must
  // go into Finish mode and complete its final row.
  eos_read_ = stream_->eosr();
  VLOG_ROW << "LZO decompressed " << uncompressed_len << " bytes from " 
           << stream_->filename() << " @" << stream_->file_offset() - compressed_len;
  return Status::OK;
}

}
