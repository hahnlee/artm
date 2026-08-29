#import "DARRuntimeClient+AppShims.h"

#import <AppKit/AppKit.h>

static NSString *const DARErrorDomain = @"dev.darwinart.manager";

@interface DARRuntimeClient (AppShimsPrivate)
- (nullable NSData *)runDataProgram:(NSString *)program
                          arguments:(NSArray<NSString *> *)arguments
                              error:(NSError **)error;
@end

@implementation DARRuntimeClient (AppShims)

- (NSURL *)appShimRootURL {
    NSString *override = NSProcessInfo.processInfo.environment[@"DARWIN_ART_APP_SHIM_ROOT"];
    if (override.length) return [NSURL fileURLWithPath:override isDirectory:YES];
    return [NSURL fileURLWithPath:
        [NSHomeDirectory() stringByAppendingPathComponent:
            @"Applications/Darwin ART Apps.localized"] isDirectory:YES];
}

- (BOOL)isManagedAppShimAtURL:(NSURL *)URL
                       profile:(NSString *_Nullable *_Nullable)profile
                       package:(NSString *_Nullable *_Nullable)package {
    NSURL *infoURL = [URL URLByAppendingPathComponent:@"Contents/Info.plist"];
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:infoURL];
    if (![info[@"DARManagedAppShim"] boolValue]) return NO;
    if (profile) *profile = info[@"DARProfile"];
    if (package) *package = info[@"DARPackage"];
    return YES;
}

- (NSString *)safeAppShimName:(NSString *)name {
    NSCharacterSet *invalid = [NSCharacterSet characterSetWithCharactersInString:@"/:"];
    NSString *safe = [[name componentsSeparatedByCharactersInSet:invalid]
        componentsJoinedByString:@"-"];
    safe = [safe stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return (!safe.length || [safe isEqualToString:@"."] || [safe isEqualToString:@".."]) ?
        @"Android App" : safe;
}

- (NSString *)bundleIdentifierComponent:(NSString *)value {
    NSMutableString *result = [NSMutableString string];
    NSCharacterSet *valid = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"];
    for (NSUInteger index = 0; index < value.length; index++) {
        unichar character = [value characterAtIndex:index];
        unichar safeCharacter = [valid characterIsMember:character] ? character : (unichar)'-';
        [result appendFormat:@"%C", safeCharacter];
    }
    return result.length ? result : @"app";
}

- (nullable NSData *)PNGIconDataForApp:(DARInstalledApp *)app {
    NSImage *image = app.iconData.length ? [[NSImage alloc] initWithData:app.iconData] : nil;
    if (!image) image = [NSImage imageNamed:NSImageNameApplicationIcon];
    NSRect rect = NSMakeRect(0, 0, 512, 512);
    CGImageRef CGImage = [image CGImageForProposedRect:&rect context:nil hints:nil];
    if (!CGImage) return nil;
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithCGImage:CGImage];
    return [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}

- (BOOL)appShimAtURL:(NSURL *)URL
        isCurrentFor:(DARInstalledApp *)app
             profile:(NSString *)profile {
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:
        [URL URLByAppendingPathComponent:@"Contents/Info.plist"]];
    if (![info[@"DARManagedAppShim"] boolValue] ||
        ![info[@"DARProfile"] isEqualToString:profile] ||
        ![info[@"DARPackage"] isEqualToString:app.packageName] ||
        ![info[@"CFBundleDisplayName"] isEqualToString:app.displayName] ||
        ![info[@"CFBundleShortVersionString"]
            isEqualToString:(app.version.length ? app.version : @"1")] ||
        ![info[@"DARLauncherVersion"] isEqual:@2] ||
        ![info[@"DARManagerBundlePath"] isEqualToString:NSBundle.mainBundle.bundleURL.path]) {
        return NO;
    }
    NSData *expectedIcon = [self PNGIconDataForApp:app];
    NSData *actualIcon = [NSData dataWithContentsOfURL:
        [URL URLByAppendingPathComponent:@"Contents/Resources/AppIcon.png"]];
    return expectedIcon.length && [expectedIcon isEqualToData:actualIcon];
}

- (BOOL)URL:(NSURL *)left refersToSamePathAsURL:(NSURL *)right {
    NSString *leftPath = left.URLByResolvingSymlinksInPath.standardizedURL.path;
    NSString *rightPath = right.URLByResolvingSymlinksInPath.standardizedURL.path;
    return [leftPath isEqualToString:rightPath];
}

- (BOOL)writeAppShimForApp:(DARInstalledApp *)app
                    profile:(NSString *)profile
                      atURL:(NSURL *)destination
                      error:(NSError **)error {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *launcher = [NSBundle.mainBundle.resourceURL
        URLByAppendingPathComponent:@"DarwinARTAppLauncher"];
    if (![files isExecutableFileAtPath:launcher.path]) {
        if (error) *error = [NSError errorWithDomain:DARErrorDomain code:3 userInfo:@{
            NSLocalizedDescriptionKey: @"앱 shim 실행 파일이 관리 앱 번들에 없습니다."
        }];
        return NO;
    }
    NSURL *root = destination.URLByDeletingLastPathComponent;
    NSURL *staging = [root URLByAppendingPathComponent:
        [NSString stringWithFormat:@".darwin-art-%@.app", NSUUID.UUID.UUIDString]
                                           isDirectory:YES];
    NSURL *contents = [staging URLByAppendingPathComponent:@"Contents" isDirectory:YES];
    NSURL *macOS = [contents URLByAppendingPathComponent:@"MacOS" isDirectory:YES];
    NSURL *resources = [contents URLByAppendingPathComponent:@"Resources" isDirectory:YES];
    if (![files createDirectoryAtURL:macOS withIntermediateDirectories:YES attributes:nil error:error] ||
        ![files createDirectoryAtURL:resources withIntermediateDirectories:YES attributes:nil error:error]) {
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    NSURL *shimExecutable = [macOS URLByAppendingPathComponent:@"DarwinARTAppLauncher"];
    if (![files copyItemAtURL:launcher toURL:shimExecutable error:error]) {
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    [files setAttributes:@{NSFilePosixPermissions: @0755}
            ofItemAtPath:shimExecutable.path error:nil];
    NSData *icon = [self PNGIconDataForApp:app];
    if (icon && ![icon writeToURL:[resources URLByAppendingPathComponent:@"AppIcon.png"]
                           options:NSDataWritingAtomic error:error]) {
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    NSString *bundleID = [NSString stringWithFormat:@"dev.darwinart.app.%@.%@",
        [self bundleIdentifierComponent:profile],
        [self bundleIdentifierComponent:app.packageName]];
    NSDictionary *info = @{
        @"CFBundleDevelopmentRegion": @"ko",
        @"CFBundleDisplayName": app.displayName,
        @"CFBundleExecutable": @"DarwinARTAppLauncher",
        @"CFBundleIconFile": @"AppIcon.png",
        @"CFBundleIdentifier": bundleID,
        @"CFBundleInfoDictionaryVersion": @"6.0",
        @"CFBundleName": app.displayName,
        @"CFBundlePackageType": @"APPL",
        @"CFBundleShortVersionString": app.version.length ? app.version : @"1",
        @"CFBundleVersion": @"1",
        @"LSMinimumSystemVersion": @"14.0",
        @"LSUIElement": @YES,
        @"NSHighResolutionCapable": @YES,
        @"DARManagedAppShim": @YES,
        @"DARLauncherVersion": @2,
        @"DARProfile": profile,
        @"DARPackage": app.packageName,
        @"DARManagerBundlePath": NSBundle.mainBundle.bundleURL.path,
    };
    if (![info writeToURL:[contents URLByAppendingPathComponent:@"Info.plist"]
                atomically:YES]) {
        if (error) *error = [NSError errorWithDomain:DARErrorDomain code:4 userInfo:@{
            NSLocalizedDescriptionKey: @"앱 shim Info.plist를 쓸 수 없습니다."
        }];
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    NSError *signError = nil;
    [self runDataProgram:@"/usr/bin/codesign"
               arguments:@[@"--force", @"--sign", @"-", @"--timestamp=none", staging.path]
                   error:&signError];
    if (signError) {
        if (error) *error = signError;
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    if ([files fileExistsAtPath:destination.path] &&
        ![files removeItemAtURL:destination error:error]) {
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    if (![files moveItemAtURL:staging toURL:destination error:error]) {
        [files removeItemAtURL:staging error:nil];
        return NO;
    }
    return YES;
}

- (void)removeAppShimsForProfile:(NSString *)profile {
    NSFileManager *files = NSFileManager.defaultManager;
    NSArray<NSURL *> *entries = [files contentsOfDirectoryAtURL:self.appShimRootURL
                                    includingPropertiesForKeys:nil options:0 error:nil];
    for (NSURL *entry in entries) {
        NSString *entryProfile = nil;
        if ([self isManagedAppShimAtURL:entry profile:&entryProfile package:nil] &&
            [entryProfile isEqualToString:profile]) {
            [files removeItemAtURL:entry error:nil];
        }
    }
}

- (BOOL)synchronizeAppShims:(NSArray<DARInstalledApp *> *)apps
                     profile:(NSString *)profile
                       error:(NSError **)error {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *root = self.appShimRootURL;
    if (![files createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:error]) {
        return NO;
    }
    NSArray<NSURL *> *entries = [files contentsOfDirectoryAtURL:root
                                    includingPropertiesForKeys:nil options:0 error:error];
    if (!entries) return NO;
    NSMutableDictionary<NSString *, NSURL *> *existing = [NSMutableDictionary dictionary];
    for (NSURL *entry in entries) {
        NSString *entryProfile = nil;
        NSString *package = nil;
        if ([self isManagedAppShimAtURL:entry profile:&entryProfile package:&package] &&
            [entryProfile isEqualToString:profile] && package.length) existing[package] = entry;
    }
    NSCountedSet *labels = [[NSCountedSet alloc] init];
    for (DARInstalledApp *app in apps) [labels addObject:app.displayName];
    NSMutableSet<NSString *> *desiredPackages = [NSMutableSet set];
    for (DARInstalledApp *app in apps) {
        [desiredPackages addObject:app.packageName];
        NSString *name = [self safeAppShimName:app.displayName];
        if (![profile isEqualToString:@"default"]) {
            name = [NSString stringWithFormat:@"%@ (%@)", name, [self safeAppShimName:profile]];
        }
        if ([labels countForObject:app.displayName] > 1) {
            name = [NSString stringWithFormat:@"%@ (%@)", name, app.packageName];
        }
        NSURL *destination = [root URLByAppendingPathComponent:
            [name stringByAppendingPathExtension:@"app"] isDirectory:YES];
        NSString *occupiedProfile = nil;
        NSString *occupiedPackage = nil;
        BOOL managed = [self isManagedAppShimAtURL:destination profile:&occupiedProfile
                                            package:&occupiedPackage];
        if ([files fileExistsAtPath:destination.path] &&
            !(managed && [occupiedProfile isEqualToString:profile] &&
              [occupiedPackage isEqualToString:app.packageName])) {
            NSString *qualified = [NSString stringWithFormat:@"%@ (%@).app", name, app.packageName];
            destination = [root URLByAppendingPathComponent:qualified isDirectory:YES];
        }
        NSURL *old = existing[app.packageName];
        BOOL current = [self appShimAtURL:destination isCurrentFor:app profile:profile];
        if (!current &&
            ![self writeAppShimForApp:app profile:profile atURL:destination error:error]) return NO;
        if (old && ![self URL:old refersToSamePathAsURL:destination]) {
            [files removeItemAtURL:old error:nil];
        }
    }
    for (NSString *package in existing) {
        if (![desiredPackages containsObject:package]) {
            [files removeItemAtURL:existing[package] error:nil];
        }
    }
    return YES;
}

@end

