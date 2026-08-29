#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface DARInstalledApp : NSObject

@property(nonatomic, copy) NSString *packageName;
@property(nonatomic, copy) NSString *displayName;
@property(nonatomic, copy) NSString *version;
@property(nonatomic, nullable) NSData *iconData;

@end

@interface DARRuntimeSnapshot : NSObject

@property(nonatomic, copy) NSArray<DARInstalledApp *> *apps;
@property(nonatomic, copy) NSDictionary<NSString *, NSArray<NSNumber *> *> *processes;
@property(nonatomic, copy) NSString *daemonStatus;
@property(nonatomic) unsigned long long allocatedBytes;

@end

typedef void (^DARRuntimeSnapshotHandler)(DARRuntimeSnapshot *_Nullable snapshot,
                                           NSError *_Nullable error);
typedef void (^DARRuntimeActionHandler)(NSError *_Nullable error);
typedef void (^DARRuntimeLogHandler)(NSString *line);
typedef void (^DARProfilesHandler)(NSArray<NSString *> *_Nullable profiles,
                                    NSError *_Nullable error);

@interface DARRuntimeClient : NSObject

@property(nonatomic, readonly) NSURL *runtimeRootURL;
@property(nonatomic, readonly) NSString *profileName;
@property(nonatomic, copy, nullable) DARRuntimeLogHandler logHandler;

- (nullable instancetype)initWithError:(NSError **)error;
- (void)fetchProfiles:(DARProfilesHandler)handler;
- (void)switchToProfile:(NSString *)profile;
- (void)createProfile:(NSString *)profile completion:(DARRuntimeActionHandler)handler;
- (void)deleteProfile:(NSString *)profile completion:(DARRuntimeActionHandler)handler;
- (void)fetchSnapshot:(DARRuntimeSnapshotHandler)handler;
- (void)installAPKAtURL:(NSURL *)url completion:(DARRuntimeActionHandler)handler;
- (void)uninstallPackage:(NSString *)package
              removeData:(BOOL)removeData
              completion:(DARRuntimeActionHandler)handler;
- (void)launchPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler;
- (void)stopPackage:(NSString *)package
                pids:(NSArray<NSNumber *> *)pids
          completion:(DARRuntimeActionHandler)handler;
- (void)revealDataForPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler;
- (void)restartSystem:(DARRuntimeActionHandler)handler;
- (void)revealProfile:(DARRuntimeActionHandler)handler;

@end

NS_ASSUME_NONNULL_END
