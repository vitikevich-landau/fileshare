#include "fileshare/client.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "fileshare/checksum.hpp"

namespace fileshare {

void Client::connect(const std::string& host, std::uint16_t port) {
    sock_ = net::tcp_connect(host, port);
}

std::vector<ListEntry> Client::request_list() {
    net::send_all(sock_, encode_list_request());
    const std::optional<Frame> frame = net::recv_message(sock_);
    if (!frame) {
        throw net::NetError("server closed the connection during LIST");
    }
    if (frame->type == MessageType::ERROR_MSG) {
        const ErrorMessage e = parse_error(frame->payload.data(), frame->payload.size());
        throw ProtocolError("server error: " + e.message);
    }
    if (frame->type != MessageType::LIST_RESPONSE) {
        throw ProtocolError("unexpected reply to LIST_REQUEST");
    }
    return parse_list_response(frame->payload.data(), frame->payload.size());
}

Client::DownloadResult Client::download(const std::string& alias, const std::string& out_path,
                                        std::uint64_t expected_size, const ProgressFn& progress) {
    DownloadResult res;

    // Stream into <out>.part and only rename onto out_path on full success, so a
    // failed download never truncates or clobbers an existing destination.
    const std::string tmp_path = out_path + ".part";

    // Resume: if a partial from a previous interrupted attempt exists, request
    // from its size and seed the hasher with the bytes already on disk, so the
    // final checksum covers the whole file (existing + newly received bytes).
    std::uint64_t offset = 0;
    FileHasher hasher;
    {
        std::error_code ec;
        if (std::filesystem::exists(tmp_path, ec) && !ec) {
            std::ifstream part(tmp_path, std::ios::binary);
            std::vector<char> pbuf(64 * 1024);
            while (part) {
                part.read(pbuf.data(), static_cast<std::streamsize>(pbuf.size()));
                const std::streamsize got = part.gcount();
                if (got > 0) {
                    hasher.update(reinterpret_cast<const std::uint8_t*>(pbuf.data()),
                                  static_cast<std::size_t>(got));
                    offset += static_cast<std::uint64_t>(got);
                }
            }
        }
    }

    net::send_all(sock_, encode_download_request(DownloadRequest{alias, offset}));

    std::ofstream out(tmp_path, offset > 0 ? (std::ios::binary | std::ios::app)
                                           : (std::ios::binary | std::ios::trunc));
    if (!out) {
        res.error = "cannot open temp file: " + tmp_path;
        return res;
    }

    std::uint64_t received = offset;

    // Keep the .part on a network drop (so a re-run resumes it); remove it on a
    // logical failure (server error, checksum mismatch, write error) where the
    // partial is useless or wrong.
    auto fail = [&](std::string message, bool keep_part) -> DownloadResult {
        out.close();
        if (!keep_part) {
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
        }
        res.error = std::move(message);
        res.bytes = received;
        return res;
    };

    for (;;) {
        const std::optional<Frame> frame = net::recv_message(sock_);
        if (!frame) {
            return fail("connection closed mid-download", /*keep_part=*/true);
        }
        switch (frame->type) {
            case MessageType::CHUNK_DATA: {
                if (!frame->payload.empty()) {
                    out.write(reinterpret_cast<const char*>(frame->payload.data()),
                              static_cast<std::streamsize>(frame->payload.size()));
                    if (!out) {
                        return fail("write error on output file", /*keep_part=*/false);
                    }
                    hasher.update(frame->payload.data(), frame->payload.size());
                    received += frame->payload.size();
                }
                if (progress) {
                    progress(received, expected_size);
                }
                break;
            }
            case MessageType::DOWNLOAD_DONE: {
                const Checksum server_sum =
                    parse_download_done(frame->payload.data(), frame->payload.size());
                const Checksum local_sum = hasher.value();
                if (server_sum != local_sum) {
                    return fail("checksum mismatch", /*keep_part=*/false);
                }
                out.flush();
                out.close();
                if (!out) {   // a disk error surfacing only at flush/close must not read as success
                    return fail("error flushing output file (disk full?)", /*keep_part=*/false);
                }
                std::error_code ec;
                std::filesystem::rename(tmp_path, out_path, ec);
                if (ec) {
                    return fail("cannot move download into place: " + ec.message(),
                                /*keep_part=*/false);
                }
                res.ok = true;
                res.bytes = received;
                res.checksum_ok = true;
                return res;
            }
            case MessageType::ERROR_MSG: {
                const ErrorMessage e = parse_error(frame->payload.data(), frame->payload.size());
                return fail("server error: " + e.message, /*keep_part=*/false);
            }
            default:
                return fail("unexpected message type during download", /*keep_part=*/false);
        }
    }
}

} // namespace fileshare
