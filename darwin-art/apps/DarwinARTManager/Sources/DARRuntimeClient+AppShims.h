#import "DARRuntimeClient.h"

NS_ASSUME_NONNULL_BEGIN

@interface DARRuntimeClient (AppShims)

- (void)removeAppShimsForProfile:(NSString *)profile;
- (BOOL)synchronizeAppShims:(NSArray<DARInstalledApp *> *)apps
                     profile:(NSString *)profile
                       error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END

