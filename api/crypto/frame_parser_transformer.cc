
#include "frame_parser_transformer.h"
#include "modules/rtp_rtcp/source/frame_object.h"
#include "rtc_base/logging.h"
#include "common_video/h264/h264_common.h"

namespace webrtc {

namespace {
class TransformableParserFrame
: public TransformableVideoFrameInterface {
public:
    TransformableParserFrame(std::unique_ptr<RtpFrameObject> frame)
        : frame_(std::move(frame)),
          metadata_(frame_->GetRtpVideoHeader().GetAsMetadata()) {
      metadata_.SetCsrcs(frame_->Csrcs());
    }
    ~TransformableParserFrame() override = default;
    
    // Implements TransformableVideoFrameInterface.
    rtc::ArrayView<const uint8_t> GetData() const override {
      return *frame_->GetEncodedData();
    }
    
    void SetData(rtc::ArrayView<const uint8_t> data) override {
      frame_->SetEncodedData(
          EncodedImageBuffer::Create(data.data(), data.size()));
    }

    uint8_t GetPayloadType() const override { return frame_->PayloadType(); }
    uint32_t GetSsrc() const override { return Metadata().GetSsrc(); }
    uint32_t GetTimestamp() const override { return frame_->RtpTimestamp(); }
    void SetRTPTimestamp(uint32_t timestamp) override {
      frame_->SetRtpTimestamp(timestamp);
    }

    bool IsKeyFrame() const override {
      return frame_->FrameType() == VideoFrameType::kVideoFrameKey;
    }

    VideoFrameMetadata Metadata() const override { return metadata_; }

    void SetMetadata(const VideoFrameMetadata& metadata) override {
      // Create |new_metadata| from existing metadata and change only frameId and
      // dependencies.
      VideoFrameMetadata new_metadata = Metadata();
      new_metadata.SetFrameId(metadata.GetFrameId());
      new_metadata.SetFrameDependencies(metadata.GetFrameDependencies());
      RTC_DCHECK(new_metadata == metadata)
          << "TransformableParserFrame::SetMetadata can be only used to "
             "change frameID and dependencies";
      frame_->SetHeaderFromMetadata(new_metadata);
    }

    const RTPVideoHeader& header () const override {
      return frame_->GetRtpVideoHeader();
    }

    std::unique_ptr<RtpFrameObject> ExtractFrame() && {
      return std::move(frame_);
    }

    Direction GetDirection() const override { return Direction::kReceiver; }
    std::string GetMimeType() const override {
      std::string mime_type = "video/";
      return mime_type + CodecTypeToPayloadString(frame_->codec_type());
    }
    
private:
    std::unique_ptr<RtpFrameObject> frame_;
    VideoFrameMetadata metadata_;
};
}

FrameParserTransformer::FrameParserTransformer(
  rtc::Thread* signaling_thread,
  const std::string participant_id)
  : signaling_thread_(signaling_thread),
    thread_(rtc::Thread::Create()),
    participant_id_(participant_id) {
    thread_->SetName("FrameParserTransformer", this);
    thread_->Start();
}

FrameParserTransformer::~FrameParserTransformer() {
    thread_->Stop();
}

void FrameParserTransformer::Transform(
  std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
    webrtc::MutexLock lock(&sink_mutex_);
    if (sink_callback_ == nullptr && sink_callbacks_.size() == 0) {
      RTC_LOG(LS_WARNING)
          << "FrameParserTransformer::Transform sink_callback_ is NULL";
      return;
    }
    
    switch (frame->GetDirection()) {
      case webrtc::TransformableFrameInterface::Direction::kSender:
        RTC_DCHECK(thread_ != nullptr);
        break;
      case webrtc::TransformableFrameInterface::Direction::kReceiver:
        RTC_DCHECK(thread_ != nullptr);
        thread_->PostTask([frame = std::move(frame), this]() mutable {
          parseFrame(std::move(frame));
        });
        break;
      case webrtc::TransformableFrameInterface::Direction::kUnknown:
        // do nothing
        RTC_LOG(LS_INFO) << "FrameCryptorTransformer::Transform() kUnknown";
        break;
    }
}

void FrameParserTransformer::parseFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
    RTC_LOG(LS_INFO) << "FrameParserTransformer::parseFrame() Start";
    rtc::scoped_refptr<webrtc::TransformedFrameCallback> sink_callback = nullptr;
    
    {
        webrtc::MutexLock lock(&mutex_);
        sink_callback = sink_callbacks_[frame->GetSsrc()];
    }
    
    if (sink_callback == nullptr) {
      RTC_LOG(LS_WARNING)
          << "FrameParserTransformer::ParseFrame() sink_callback is NULL";
      return;
    }
    
    rtc::ArrayView<const uint8_t> data_in = frame->GetData();
    
    if (data_in.size() == 0) {
      RTC_LOG(LS_WARNING) << "FrameParserTransformer::parseFrame() "
                             "data_in.size() == 0";

      sink_callback->OnTransformedFrame(std::move(frame));
      return;
    }
    
    /// NOTE: get header data
    auto headerData = data_in.subview(5, 3);
    
    auto flag = headerData[0];
    
    auto version = flag & 3;
    if (version > FRAME_MAX_VERSION) {
        RTC_LOG(LS_WARNING)
            << "FrameParserTransformer::ParseFrame() Unsupported frame version"
            << "version=" << version << "maxVersion=" << FRAME_MAX_VERSION;
        
        sink_callback->OnTransformedFrame(std::move(frame));
        return;
    }
    
    auto type = (flag >> 2) & 7;
    if (type > FRAME_TYPE_H265) {
        RTC_LOG(LS_WARNING)
            << "FrameParserTransformer::ParseFrame() Unsupported frame type"
            << "type=" << type;
        
        sink_callback->OnTransformedFrame(std::move(frame));
        return;
    }
    
    std::string videoType = (flag >> 5) & 1 ? "key" : "delta";
    uint16_t duration = headerData[1] << 8 | headerData[2];
    duration *= 1000;
    
    RTC_LOG(LS_INFO) << "FrameParserTransformer::ParseFrame() ParseHeader Info: "
        << "version=" << version << ", type=" << type << ", videoType=" << videoType << ", duration=" << duration
        << ", data size=" << data_in.size();
    
    auto data = data_in.subview(8, data_in.size());
    size_t offset = 0;
    if (offset + FRAME_LENGTH_SIZE > data.size()) {
        RTC_LOG(LS_WARNING)
            << "FrameParserTransformer::ParseFrame() Invalid data offset"
            << "offset=" << offset << ", data size=" << data.size();
        return;
    }
    
    rtc::Buffer video_data;
    rtc::Buffer sps_data;
    rtc::Buffer pps_data;
    while (offset < data.size()) {
        if (offset + FRAME_LENGTH_SIZE > data.size()) {
            RTC_LOG(LS_WARNING)
                << "FrameParserTransformer::ParseFrame() Invalid data offset"
                << "offset=" << offset << ", data size=" << data.size();
            return;
        }
        
        auto flag = data[offset];
        auto typ = flag & 0xF;
        uint32_t length = data[offset + 1] << 16 | data[offset + 2] << 8 | data[offset + 3];
        
        if (offset + length > data.size()) {
            RTC_LOG(LS_WARNING)
                << "FrameParserTransformer::ParseFrame() Invalid data length"
                << "offset=" << offset << ", length=" << length << ", body size=" << data.size();
            return;
        }
        
        if (typ == 0) {
            uint32_t videoDataLength = length - FRAME_LENGTH_SIZE;
            
            uint8_t  lengthData[4] = {0 ,0,0, 1};
//            lengthData[0] = (videoDataLength >> 24) & 0xFF;
//            lengthData[1] = (videoDataLength >> 16) & 0xFF;
//            lengthData[2] = (videoDataLength >> 8) & 0xFF;
//            lengthData[3] = videoDataLength & 0xFF;
            
            auto videoBody = data.subview(offset + FRAME_LENGTH_SIZE, videoDataLength);
            video_data.AppendData(lengthData);
            video_data.AppendData(videoBody);
        } else if (typ == 1) {
            auto spsBody = data.subview(offset + FRAME_LENGTH_SIZE, length - FRAME_LENGTH_SIZE);
            uint8_t  lengthData[4] = {0 ,0,0, 1};
//            lengthData[0] = (length >> 24) & 0xFF;
//            lengthData[1] = (length >> 16) & 0xFF;
//            lengthData[2] = (length >> 8) & 0xFF;
//            lengthData[3] = length & 0xFF;
            sps_data.AppendData(lengthData);
            sps_data.AppendData(spsBody);
        } else if (typ == 2) {
            auto ppsBody = data.subview(offset + FRAME_LENGTH_SIZE, length - FRAME_LENGTH_SIZE);
            uint8_t  lengthData[4] = {0 ,0,0, 1};
//            lengthData[0] = (length >> 24) & 0xFF;
//            lengthData[1] = (length >> 16) & 0xFF;
//            lengthData[2] = (length >> 8) & 0xFF;
//            lengthData[3] = length & 0xFF;
            pps_data.AppendData(lengthData);
            pps_data.AppendData(ppsBody);
        }
        
        offset += length;
        
        RTC_LOG(LS_INFO) << "FrameParserTransformer::ParseFrame() ParseData Flag Type: "
            << "typ=" << typ << ", length=" << length << ", body size=" << data.size();
    }
    
    
    rtc::Buffer data_out;
//    data_out.AppendData(data_in.subview(0, 5));
    data_out.AppendData(sps_data);
    data_out.AppendData(pps_data);
    data_out.AppendData(video_data);
    frame->SetData(data_out);
    
    auto transformed_frame = absl::WrapUnique(
        static_cast<TransformableParserFrame*>(frame.release()));
    
    std::unique_ptr<RtpFrameObject> frame_object =
        std::move(*transformed_frame).ExtractFrame();
    
    auto newMetadata = VideoFrameMetadata(frame_object->GetRtpVideoHeader().GetAsMetadata());
    if (videoType == "key") {
        auto oldFrameType = newMetadata.GetFrameType();
        newMetadata.SetFrameType(VideoFrameType::kVideoFrameKey);
    }
    frame_object->SetHeaderFromMetadata(newMetadata);
    
    RTC_LOG(LS_INFO) << "FrameParserTransformer::ParseFrame() Finish "
        << "frame_type="<< frame_object->GetRtpVideoHeader().frame_type
        << ", codec=" << frame_object->codec_type();
    
    std::vector<webrtc::H264::NaluIndex> nalu_indices =
        webrtc::H264::FindNaluIndices(data_out.data(), data_out.size());

    int idx = 0;
    for (const auto& index : nalu_indices) {
      const uint8_t* slice = data_out.data() + index.payload_start_offset;
      webrtc::H264::NaluType nalu_type =
          webrtc::H264::ParseNaluType(slice[0]);
        RTC_LOG(LS_INFO) << "FrameParserTransformer::ParseFrame() NaluType "
            << "nalu_type=" << nalu_type
            << ", start_offset=" << index.start_offset
            << ", payload_start_offset=" << index.payload_start_offset
            << ", payload_size=" << index.payload_size;
    }
    
    sink_callback->OnTransformedFrame(
        std::make_unique<TransformableParserFrame>(std::move(frame_object)));
}

}
