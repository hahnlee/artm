#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include "darwin_window_bridge.h"

@interface DarwinArtFrameView : NSView
- (instancetype)initWithFrame:(NSRect)frame image:(CGImageRef)image;
@end

@implementation DarwinArtFrameView {
  CGImageRef _image;
}

- (instancetype)initWithFrame:(NSRect)frame image:(CGImageRef)image {
  self = [super initWithFrame:frame];
  if (self != nil) {
    _image = CGImageRetain(image);
  }
  return self;
}

- (void)dealloc {
  CGImageRelease(_image);
}

- (BOOL)isFlipped {
  return YES;
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  CGContextSaveGState(context);
  CGContextTranslateCTM(context, 0.0, self.bounds.size.height);
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);
  CGContextDrawImage(context, NSRectToCGRect(self.bounds), _image);
  CGContextRestoreGState(context);
}

@end

bool DarwinPresentArgb(const std::uint32_t* pixels,
                       std::size_t width,
                       std::size_t height,
                       double visible_seconds) {
  if (pixels == nullptr || width == 0 || height == 0 ||
      width > 4096 || height > 4096) {
    return false;
  }
  if (visible_seconds <= 0.0) {
    return true;
  }
  if (![NSThread isMainThread]) {
    return false;
  }

  @autoreleasepool {
    const std::size_t byte_count = width * height * sizeof(std::uint32_t);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8*>(pixels),
                                  static_cast<CFIndex>(byte_count));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(
        static_cast<std::uint32_t>(kCGBitmapByteOrder32Little) |
        static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst));
    CGImageRef image = CGImageCreate(width, height, 8, 32,
                                     width * sizeof(std::uint32_t), color_space,
                                     bitmap_info, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CFRelease(data);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
    if (image == nullptr) {
      return false;
    }

    NSApplication* application = NSApplication.sharedApplication;
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"Darwin ART · Activity.setContentView()";
    window.contentView = [[DarwinArtFrameView alloc] initWithFrame:frame
                                                             image:image];
    [window center];
    [window makeKeyAndOrderFront:nil];
    [application activateIgnoringOtherApps:YES];
    [window displayIfNeeded];
    CGImageRelease(image);

    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:visible_seconds];
    while (deadline.timeIntervalSinceNow > 0.0 && window.visible) {
      NSDate* slice = [NSDate dateWithTimeIntervalSinceNow:0.016];
      NSEvent* event = [application nextEventMatchingMask:NSEventMaskAny
                                                untilDate:slice
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
      if (event != nil) {
        [application sendEvent:event];
      }
      [application updateWindows];
    }
    [window close];
  }
  return true;
}
