//
//  frame_parser_transformer.h
//  sources
//
//  Created by BigSaimo on 2024/6/26.
//

#ifndef WEBRTC_FRAME_PARSER_TRANSFORMER_H_
#define WEBRTC_FRAME_PARSER_TRANSFORMER_H_

#include <unordered_map>

#include "api/frame_transformer_interface.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/task_queue/task_queue_base.h"
#include "rtc_base/buffer.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/system/rtc_export.h"
#include "rtc_base/thread.h"

namespace webrtc {

const uint8_t FRAME_MAX_VERSION = 0;

const uint8_t FRAME_TYPE_H264 = 0;
const uint8_t FRAME_TYPE_H265 = 1;

const uint8_t FRAME_LENGTH_SIZE = 5;

class RTC_EXPORT FrameParserTransformer
    : public rtc::RefCountedObject<webrtc::FrameTransformerInterface> {

    public:
        explicit FrameParserTransformer(
            rtc::Thread* signaling_thread,
            const std::string participant_id);
        ~FrameParserTransformer();
        
        virtual const std::string participant_id() const { return participant_id_; }
    
    protected:
        virtual void RegisterTransformedFrameCallback(
            rtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override {
          webrtc::MutexLock lock(&sink_mutex_);
          sink_callback_ = callback;
        }
        virtual void UnregisterTransformedFrameCallback() override {
          webrtc::MutexLock lock(&sink_mutex_);
          sink_callback_ = nullptr;
        }
        virtual void RegisterTransformedFrameSinkCallback(
            rtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
            uint32_t ssrc) override {
          webrtc::MutexLock lock(&sink_mutex_);
          sink_callbacks_[ssrc] = callback;
        }
        virtual void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
          webrtc::MutexLock lock(&sink_mutex_);
          auto it = sink_callbacks_.find(ssrc);
          if (it != sink_callbacks_.end()) {
            sink_callbacks_.erase(it);
          }
        }
        
        virtual void Transform(
            std::unique_ptr<webrtc::TransformableFrameInterface> frame) override;
        
    private:
        void parseFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame);
        
    private:
        TaskQueueBase* const signaling_thread_;
        std::unique_ptr<rtc::Thread> thread_;
        std::string participant_id_;
        mutable webrtc::Mutex mutex_;
        mutable webrtc::Mutex sink_mutex_;
        rtc::scoped_refptr<webrtc::TransformedFrameCallback> sink_callback_;
        std::map<uint32_t, rtc::scoped_refptr<webrtc::TransformedFrameCallback>>
            sink_callbacks_;
        
    };

}

#endif /* WEBRTC_FRAME_PARSER_TRANSFORMER_H_ */
