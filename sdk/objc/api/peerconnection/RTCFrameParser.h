//
//  RTCFrameParser.h
//  sources
//
//  Created by BigSaimo on 2024/6/26.
//

#import <Foundation/Foundation.h>

#import "RTCMacros.h"

NS_ASSUME_NONNULL_BEGIN

@class RTC_OBJC_TYPE(RTCRtpSender);
@class RTC_OBJC_TYPE(RTCRtpReceiver);
@class RTC_OBJC_TYPE(RTCPeerConnectionFactory);

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCFrameParser) : NSObject

@property(nonatomic, readonly) NSString *participantId;

- (nullable instancetype)initWithFactory:(RTC_OBJC_TYPE(RTCPeerConnectionFactory) *)factory
                             rtpReceiver:(RTC_OBJC_TYPE(RTCRtpReceiver) *)receiver
                           participantId:(NSString *)participantId;

@end

NS_ASSUME_NONNULL_END
