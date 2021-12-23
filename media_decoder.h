#pragma once

#include <memory>
#include <string>

struct AVFrame;
struct AVStream;
struct AVDRMFrameDescriptor;

namespace pivid {

class DecoderError : public std::exception {};

class DecodedFrame {
  public:
    virtual ~DecodedFrame() {}
    virtual AVFrame const& frame() = 0;
    virtual AVDRMFrameDescriptor const& drm() = 0;
};

class MediaDecoder {
  public:
    virtual ~MediaDecoder() {}
    virtual AVStream const& stream() = 0;
    virtual std::unique_ptr<DecodedFrame> next_frame() = 0;
    virtual bool at_eof() const = 0;
};

std::unique_ptr<MediaDecoder> new_media_decoder(std::string const& url);

}  // namespace pivid