#import <AppKit/AppKit.h>

@class DARRuntimeClient;

NS_ASSUME_NONNULL_BEGIN
@interface DARMainWindowController : NSWindowController
- (instancetype)initWithRuntimeClient:(DARRuntimeClient *)client;
- (void)installAPKURLs:(NSArray<NSURL *> *)URLs;
@end
NS_ASSUME_NONNULL_END
