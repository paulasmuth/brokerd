/**
 * This file is part of the "brokerd" project
 *   Copyright (c) 2015 Paul Asmuth
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <assert.h>
#include <unistd.h>
#include <cstring>
#include <brokerd/channel.h>
#include <brokerd/util/stringutil.h>
#include <brokerd/util/file.h>
#include <brokerd/util/varint.h>
#include <brokerd/util/fileutil.h>

namespace brokerd {

Option<ChannelID> ChannelID::fromString(const std::string& s) {
  if (s.empty() || !StringUtil::isShellSafe(s)) {
    return None<ChannelID>();
  }

  return Some(ChannelID(s));
}

ChannelID::ChannelID(const std::string& id) : id_(id) {}

const std::string& ChannelID::str() const {
  return id_;
}

ChannelSegmentHandle::ChannelSegmentHandle() : fd(-1) {}

ChannelSegmentHandle::~ChannelSegmentHandle() {
  if (fd >= 0) {
    ::close(fd);
  }
}

ReturnCode Channel::createChannel(
    const std::string& path,
    std::shared_ptr<Channel>* channel) {
  std::unique_ptr<ChannelSegmentHandle> segment;
  auto rc = segmentCreate(path, 0, &segment);
  if (!rc.isSuccess()) {
    return rc;
  }

  channel->reset(new Channel(path, {}, std::move(segment)));
  return ReturnCode::success();
}

ReturnCode Channel::openChannel(
    const std::string& path,
    std::list<ChannelSegment> segments,
    std::shared_ptr<Channel>* channel) {
  std::unique_ptr<ChannelSegmentHandle> segment;
  auto rc = segmentOpen(path, segments.back(), &segment);
  if (!rc.isSuccess()) {
    return rc;
  }

  segments.pop_back();
  channel->reset(new Channel(path, segments, std::move(segment)));
  return ReturnCode::success();
}

Channel::Channel(
    const std::string& path,
    std::list<ChannelSegment> segments_archive,
    std::unique_ptr<ChannelSegmentHandle> segment_handle) :
    path_(path),
    segments_archive_(segments_archive),
    segment_handle_(std::move(segment_handle)) {}

ReturnCode Channel::appendMessage(const std::string& message, uint64_t* offset) {
  std::unique_lock<std::mutex> lk(mutex_);

  auto segment_size =
      segment_handle_->segment.offset_head -
      segment_handle_->segment.offset_begin;

  if (segment_size > kMaxSegmentSize) {
    auto rc = commitWithLock();
    if (!rc.isSuccess()) {
      return rc;
    }

    std::unique_ptr<ChannelSegmentHandle> s(new ChannelSegmentHandle());
    rc = segmentCreate(path_, segment_handle_->segment.offset_head, &s);
    if (!rc.isSuccess()) {
      return rc;
    }

    segments_archive_.emplace_back(ChannelSegment {
      .offset_begin = segment_handle_->segment.offset_begin,
      .offset_head = segment_handle_->segment.offset_head
    });

    segment_handle_ = std::move(s);
  }

  *offset = segment_handle_->segment.offset_head;

  auto rc = segmentAppend(segment_handle_.get(), message.data(), message.size());
  if (!rc.isSuccess()) {
    return rc;
  }

  needs_commit_ = true;

  return commitWithLock();
}

ReturnCode Channel::fetchMessages(
    uint64_t start_offset,
    int batch_size,
    std::list<Message>* entries) {
  entries->clear();

  std::vector<ChannelSegment> segments;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    segments.insert(
        segments.end(),
        segments_archive_.begin(),
        segments_archive_.end());

    segments.push_back(ChannelSegment {
      .offset_begin = segment_handle_->segment.offset_begin,
      .offset_head = segment_handle_->segment.offset_head,
    });
  }

  for (const auto& s : segments) {
    while (start_offset < s.offset_head) {
      auto rc = segmentRead(
          s,
          path_,
          start_offset,
          batch_size - entries->size(),
          entries);

      if (!rc.isSuccess()) {
        return rc;
      }

      if (entries->empty() || entries->size() == batch_size) {
        return ReturnCode::success();
      }

      start_offset = entries->back().next_offset;
    }
  }

  return ReturnCode::success();
}

ReturnCode Channel::commit() {
  std::unique_lock<std::mutex> lk(mutex_);
  return commitWithLock();
}

ReturnCode Channel::commitWithLock() {
  if (!needs_commit_) {
    return ReturnCode::success();
  }

  auto rc = segmentCommit(segment_handle_.get());
  if (rc.isSuccess()) {
    needs_commit_ = false;
  }

  return rc;
}

ReturnCode segmentCreate(
    const std::string& channel_path,
    uint64_t start_offset,
    std::unique_ptr<ChannelSegmentHandle>* segment) {
  auto segment_path = StringUtil::format("$0~$1", channel_path, start_offset); 
  auto segment_file = File::openFile(
    segment_path + "~",
    File::O_READ | File::O_WRITE | File::O_CREATEOROPEN | File::O_TRUNCATE,
    0644);

  ChannelSegmentTransaction tx;
  tx.offset_head = start_offset;

  std::string tx_buf;
  transactionEncode(tx, &tx_buf);

  std::string segment_header;
  segment_header.append((const char*) kMagicBytes.data(), kMagicBytes.size());
  segment_header.append((const char*) kVersion.data(), kVersion.size());
  segment_header.append(tx_buf);

  assert(segment_header.size() <= kSegmentHeaderSize);

  if (segment_header.size() < kSegmentHeaderSize) {
    segment_header.append(std::string(kSegmentHeaderSize - segment_header.size(), 0));
  }

  {
    auto rc = ::write(
        segment_file.fd(),
        segment_header.data(),
        segment_header.size());

    if (rc < 0 || rc != segment_header.size()) {
      return ReturnCode::errorf(
          "EIO",
          "can't write segment header to '$0': $1",
          segment_path,
          strerror(errno));
    }
  }

  FileUtil::mv(segment_path + "~", segment_path);

  std::unique_ptr<ChannelSegmentHandle> s(new ChannelSegmentHandle());
  s->segment.offset_begin = start_offset;
  s->segment.offset_head = start_offset;
  s->fd = segment_file.releaseFD();

  *segment = std::move(s);
  return ReturnCode::success();
}

ReturnCode segmentOpen(
    const std::string& channel_path,
    const ChannelSegment& segment,
    std::unique_ptr<ChannelSegmentHandle>* segment_handle) {
  auto segment_path = StringUtil::format(
      "$0~$1",
      channel_path,
      segment.offset_begin);

  auto segment_file_offset =
      (segment.offset_head - segment.offset_begin) + kSegmentHeaderSize;

  auto segment_file = File::openFile(segment_path, File::O_WRITE);
  segment_file.seekTo(segment_file_offset);
  segment_handle->reset(new ChannelSegmentHandle());
  segment_handle->get()->segment = segment;
  segment_handle->get()->fd = segment_file.releaseFD();

  return ReturnCode::success();
}

ReturnCode segmentAppend(
    ChannelSegmentHandle* segment,
    const char* message,
    size_t message_len) {

  size_t message_envelope_size;
  auto rc = messageWrite(
      message,
      message_len,
      segment->fd,
      &message_envelope_size);

  if (!rc.isSuccess()) {
    return rc;
  }

  segment->segment.offset_head += message_envelope_size;
  return ReturnCode::success();
}

ReturnCode segmentCommit(ChannelSegmentHandle* segment) {
  ChannelSegmentTransaction tx;
  tx.offset_head = segment->segment.offset_head;

  std::string tx_buf;
  transactionEncode(tx, &tx_buf);

  if (::fdatasync(segment->fd) == -1) {
    return ReturnCode::errorf("EIO", "fsync() failed: $0", strerror(errno));
  }

  int rc = ::pwrite(
      segment->fd,
      tx_buf.data(),
      tx_buf.size(),
      kSegmentHeaderTransactionOffset);

  if (rc < 0 || rc != tx_buf.size()) {
    return ReturnCode::errorf("EIO", "write() failed: $0", strerror(errno));
  }

  return ReturnCode::success();
}

ReturnCode segmentRead(
    const ChannelSegment& segment,
    const std::string& channel_path,
    uint64_t start_offset,
    size_t batch_size,
    std::list<Message>* entries) {
  if (start_offset < segment.offset_begin ||
      start_offset >= segment.offset_head) {
    return ReturnCode::error("EARG", "offset is out of bounds");
  }

  auto segment_path = StringUtil::format(
      "$0~$1",
      channel_path,
      segment.offset_begin);

  auto segment_file = File::openFile(segment_path, File::O_READ);
  auto segment_file_offset = start_offset - segment.offset_begin;
  auto segment_file_len = segment.offset_head - segment.offset_begin;

  Message msg;
  uint64_t msg_len = 0;
  while (segment_file_offset < segment_file_len) {
    std::array<uint8_t, 4096> buf;
    int rc = ::pread(
        segment_file.fd(),
        buf.data(),
        buf.size(),
        segment_file_offset + kSegmentHeaderSize);

    if (rc <= 0 || rc > buf.size()) {
      return ReturnCode::errorf("EIO", "read() failed: $0", strerror(errno));
    }

    auto begin = (const char*) &buf[0];
    auto cur = begin;
    auto end = (const char*) cur + rc;
    while (cur < end) {
      if (!msg_len) {
        auto message_offset = segment_file_offset + (cur - begin);

        if (!readVarUInt(&cur, end, &msg_len)) {
          return ReturnCode::errorf("EIO", "corrupt file: $0", segment_path);
        }

        msg.offset = message_offset + segment.offset_begin;
        msg.next_offset = segment_file_offset + (cur - begin) + msg_len + segment.offset_begin;
      }

      if (msg_len > end - cur) {
        msg.data.append(cur, end - cur);
        msg_len -= end - cur;
        cur = end;
        break;
      } else {
        msg.data.append(std::string(cur, msg_len));
        entries->emplace_back(std::move(msg));
        cur += msg_len;
        msg_len = 0;
      }

      if (--batch_size == 0) {
        return ReturnCode::success();
      }
    }

    segment_file_offset += rc;
  }

  return ReturnCode::success();
}

ReturnCode segmentReadHeader(
    const std::string& channel_path,
    uint64_t start_offset,
    ChannelSegment* segment) {
  auto segment_path = StringUtil::format(
      "$0~$1",
      channel_path,
      start_offset);

  auto segment_file = File::openFile(segment_path, File::O_READ);

  std::array<uint8_t, kSegmentHeaderSize> buf;
  size_t buf_len = 0;
  while (buf_len < kSegmentHeaderSize) {
    int rc = ::read(
        segment_file.fd(),
        buf.data() + buf_len,
        buf.size() - buf_len);

    if (rc <= 0) {
      return ReturnCode::errorf("EIO", "read() failed: $0", strerror(errno));
    } else {
      buf_len += rc;
    }
  }

  if (memcmp(buf.data(), kMagicBytes.data(), kMagicBytes.size()) != 0) {
    return ReturnCode::errorf("EIO", "corrupt file: $0", segment_path);
  }

  ChannelSegmentTransaction tx;
  auto rc = transactionDecode(
      (const char*) buf.data() + kSegmentHeaderTransactionOffset,
      buf.size() - kSegmentHeaderTransactionOffset,
      &tx);

  if (!rc.isSuccess()) {
    return ReturnCode::errorf("EIO", "corrupt file '$0': $1", rc.getMessage());
  }

  segment->offset_begin = start_offset;
  segment->offset_head = tx.offset_head;

  return ReturnCode::success();
}

void transactionEncode(
    const ChannelSegmentTransaction& tx,
    std::string* buf) {
  std::string b(sizeof(tx.offset_head), 0);
  memcpy(&b[0], &tx.offset_head, sizeof(tx.offset_head));
  buf->append(b);
}

ReturnCode transactionDecode(
    const char* buf,
    size_t buf_len,
    ChannelSegmentTransaction* tx) {
  if (buf_len < sizeof(tx->offset_head)) {
    return ReturnCode::error("EIO", "invalid header");
  }

  memcpy(&tx->offset_head, buf, sizeof(tx->offset_head));
  return ReturnCode::success();
}

} // namespace brokerd

