#include "coring5/proto/http1conn/Responders.hpp"
#include <fcntl.h>

namespace Coring5 {
namespace {
uint16_t file_status(int error) {
  switch (error) {
  case ENOENT:
  case ENOTDIR:
  case EACCES:
  case ELOOP:
  case EXDEV:
    return 404;
  case EISDIR:
    return 403;
  case ENAMETOOLONG:
    return 414;
  case EMFILE:
  case ENFILE:
  case ENOMEM:
    return 503;
  default:
    return 500;
  }
}
} // namespace

void FileResponder::start(IoContext &ctx, ConnCore &core, Exchange &ex,
                          const Http1::RequestHead &head,
                          const StaticRoute &route) {
  if (head.method != "GET" && head.method != "HEAD") {
    static constexpr ResponseField allow{"Allow", "GET, HEAD"};
    (void)Respond::apply(ex, cursor_, 405, {}, std::span(&allow, 1));
    return;
  }
  auto buf = buffer_.bytes();
  if (!ctx.static_roots || buf.empty()) {
    (void)ex.respond_synthetic(503);
    return;
  }
  const auto root = ctx.static_roots->directories.find(route.root);
  if (root == ctx.static_roots->directories.end()) {
    (void)ex.respond_synthetic(503);
    return;
  }
  const auto path = StaticFileRoot::decode_target(head.target, buf);
  if (!path) {
    (void)ex.respond_synthetic(path.error().code == Errc::TooLarge ? 414 : 400);
    return;
  }
  // Preserve conditional fields across asynchronous lookup and config reload.
  const std::string_view uri(buf.data());
  auto tail = buf.subspan(uri.size() + 1);
  // Values plus separators fit in the stage: their original wire form also
  // contained the field names, colons and CRLFs.
  auto save_list =
      [&](std::string_view name) -> std::optional<std::string_view> {
    const char *begin = tail.data();
    bool found = false;
    for (const auto &field : std::span(head.headers).first(head.header_count)) {
      if (!field.named(name)) {
        continue;
      }
      if (found) {
        tail.front() = ',';
        tail = tail.subspan(1);
      }
      std::ranges::copy(field.value, tail.begin());
      tail = tail.subspan(field.value.size());
      found = true;
    }
    if (!found) {
      return std::nullopt;
    }
    return std::string_view(begin, tail.data());
  };
  conditions_.if_none_match = save_list("if-none-match");
  conditions_.if_modified_since = save_list("if-modified-since");
  conditions_.range = save_list("range");
  conditions_.has_if_range = head.find("if-range").data() != nullptr;

  // Keep the pinned root open across reload and all candidate probes.
  directory_.reset(::fcntl(root->second.fd(), F_DUPFD_CLOEXEC, 0));
  if (!directory_) {
    (void)ex.respond_synthetic(503);
    return;
  }
  options_ = route.options;
  lookup_.emplace(uri, options_.get());
  open_next(ctx, core, ex);
}

void FileResponder::open_next(IoContext &ctx, ConnCore &core, Exchange &ex) {
  const auto path = lookup_->next();
  if (!path) {
    fail_lookup(core, ex, file_status(path.error().sys));
    return;
  }
  core.reset_peer();
  if (!core.counted_file_lookup(
          ctx.ring.open_file(directory_.get(), *path, &how_,
                             core.tag(OpKind::FileOpen)),
          OpKind::FileOpen)) {
    core.close(ctx, Errc::Overloaded);
  }
}

void FileResponder::fail_lookup(ConnCore &core, Exchange &ex, uint16_t status) {
  directory_.reset();
  core.reset_peer();
  (void)ex.respond_synthetic(status);
}

void FileResponder::complete(IoContext &ctx, ConnCore &core, Exchange &ex,
                             const Cqe &cqe) {
  if (cqe.tag.kind() == OpKind::FileOpen) {
    if (cqe.res < 0) {
      if (StaticFileLookup::is_miss(-cqe.res)) {
        open_next(ctx, core, ex);
      } else {
        fail_lookup(core, ex, file_status(-cqe.res));
      }
      return;
    }
    core.set_peer(FdOwner(cqe.res));
    stat_ = {};
    if (!core.counted_file_lookup(
            ctx.ring.stat_file(core.peer_fd(), StaticFileRoot::stat_fields,
                               &stat_, core.tag(OpKind::FileStat)),
            OpKind::FileStat)) {
      core.close(ctx, Errc::Overloaded);
    }
  } else if (cqe.tag.kind() == OpKind::FileStat) {
    if (cqe.res < 0) {
      fail_lookup(core, ex, file_status(-cqe.res));
      return;
    }
    const auto meta = lookup_->inspect(stat_);
    if (!meta) {
      fail_lookup(core, ex, 404);
      return;
    }
    if (!*meta) {
      open_next(ctx, core, ex);
      return;
    }
    directory_.reset();
    file_ =
        StaticFile::prepare(**meta, ex.request_meta().head_only, conditions_);
    fields_ = {{{"Last-Modified", file_->last_modified()},
                {"ETag", file_->etag()},
                {"Accept-Ranges", file_->accept_ranges()},
                {"Content-Range", file_->content_range()}}};
  } else {
    reading_ = false;
    const auto data = std::as_bytes(buffer_.bytes());
    if (cqe.res < 0 || static_cast<size_t>(cqe.res) > data.size()) {
      (void)file_->on_read(ex, cursor_, unexpected_error(Errc::Internal));
    } else {
      (void)file_->on_read(ex, cursor_,
                           data.first(static_cast<size_t>(cqe.res)));
    }
  }
}

void FileResponder::pump(IoContext &ctx, ConnCore &core, Exchange &ex) {
  if (!file_ || reading_ || !core.has_peer()) {
    return;
  }
  auto buf = buffer_.bytes();
  auto step = file_->apply(
      ex, cursor_,
      std::min(ctx.buffers.buf_size(), static_cast<uint32_t>(buf.size())),
      std::span(fields_).first(file_->content_range().empty() ? 3 : 4)
          .subspan(file_->last_modified().empty() ? 1 : 0));
  if (!step || step->status == StaticFileStep::Status::Done) {
    core.reset_peer();
  } else if (step->status == StaticFileStep::Status::Read) {
    reading_ =
        core.counted(ctx.ring.read_file(core.peer_fd(), buf.data(), step->size,
                                        step->offset, core.tag(OpKind::Read)));
    if (!reading_) {
      (void)file_->on_read(ex, cursor_, unexpected_error(Errc::Overloaded));
      core.close(ctx, Errc::Overloaded);
    }
  }
}
} // namespace Coring5
