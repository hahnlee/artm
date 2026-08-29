#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface DARRuntimeSnapshot : NSObject

@property(nonatomic, copy) NSArray<NSString *> *packages;
@property(nonatomic, copy) NSDictionary<NSString *, NSArray<NSNumber *> *> *processes;
@property(nonatomic, copy) NSString *daemonStatus;

@end

typedef void (^DARRuntimeSnapshotHandler)(DARRuntimeSnapshot *_Nullable snapshot,
                                           NSError *_Nullable error);
typedef void (^DARRuntimeActionHandler)(NSError *_Nullable error);
typedef void (^DARRuntimeLogHandler)(NSString *line);

@interface DARRuntimeClient : NSObject

@property(nonatomic, readonly) NSURL *projectRootURL;
@property(nonatomic, readonly) NSString *profileName;
@property(nonatomic, copy, nullable) DARRuntimeLogHandler logHandler;

- (nullable instancetype)initWithError:(NSError **)error;
- (void)fetchSnapshot:(DARRuntimeSnapshotHandler)handler;
- (void)installAPKAtURL:(NSURL *)url completion:(DARRuntimeActionHandler)handler;
- (void)launchPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler;
- (void)stopPackage:(NSString *)package
                pids:(NSArray<NSNumber *> *)pids
          completion:(DARRuntimeActionHandler)handler;
- (void)revealDataForPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler;

@end

NS_ASSUME_NONNULL_END
