//
//  RTCFrameParser.m
//  sources
//
//  Created by BigSaimo on 2024/6/26.
//

#import "RTCFrameParser.h"
#import "RTCPeerConnectionFactory+Private.h"
#import "RTCRtpReceiver+Private.h"

#import <os/lock.h>
#include <memory>

#import "base/RTCLogging.h"
#import "helpers/NSString+StdString.h"
#include "api/crypto/frame_parser_transformer.h"
#include "api/rtp_receiver_interface.h"

@implementation RTC_OBJC_TYPE (RTCFrameParser) {
    const webrtc::RtpReceiverInterface *_receiver;
    rtc::scoped_refptr<webrtc::FrameParserTransformer> _frame_parser_transformer;
    os_unfair_lock _lock;
}

@synthesize participantId = _participantId;

- (nullable instancetype)initWithFactory:(nonnull RTCPeerConnectionFactory *)factory rtpReceiver:(nonnull RTCRtpReceiver *)receiver participantId:(nonnull NSString *)participantId {
    if (self = [super init]) {
        _lock = OS_UNFAIR_LOCK_INIT;
        
        rtc::scoped_refptr<webrtc::RtpReceiverInterface> nativeRtpReceiver = receiver.nativeRtpReceiver;
        if (nativeRtpReceiver == nullptr) return nil;
        
        rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> nativeTrack = nativeRtpReceiver->track();
        if (nativeTrack == nullptr) return nil;
        
        os_unfair_lock_lock(&_lock);
        
        _participantId = participantId;
        
        _frame_parser_transformer = rtc::scoped_refptr<webrtc::FrameParserTransformer>(new webrtc::FrameParserTransformer(factory.signalingThread, [participantId stdString]));
        
        factory.workerThread->BlockingCall([self, nativeRtpReceiver] {
            nativeRtpReceiver->SetDepacketizerToDecoderFrameTransformer(_frame_parser_transformer);
        });
        
        os_unfair_lock_unlock(&_lock);
    }
    
    return self;
}

- (void)dealloc {
  os_unfair_lock_lock(&_lock);
  if (_frame_parser_transformer != nullptr) {
      _frame_parser_transformer = nullptr;
  }
  os_unfair_lock_unlock(&_lock);
}

@end

