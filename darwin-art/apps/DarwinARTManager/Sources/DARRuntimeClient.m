#import "DARRuntimeClient.h"

#import <AppKit/AppKit.h>

static NSString *const DARErrorDomain = @"dev.darwinart.manager";

@implementation DARRuntimeSnapshot
@end

@interface DARRuntimeClient ()
@property(nonatomic, readwrite) NSURL *runtimeRootURL;
@property(nonatomic, readwrite) NSString *profileName;
@property(nonatomic) NSMutableSet<NSTask *> *launchTasks;
@property(nonatomic) NSMapTable<NSTask *, NSURL *> *launchRecords;
@end

@implementation DARRuntimeClient

- (nullable instancetype)initWithError:(NSError **)error {
    self = [super init];
    if (!self) return nil;

    NSURL *root = [self discoverRuntimeRoot];
    if (!root) {
        if (error) {
            *error = [NSError errorWithDomain:DARErrorDomain
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"앱에 포함된 Darwin ART 런타임이 없거나 손상되었습니다."}];
        }
        return nil;
    }
    _runtimeRootURL = root;
    _profileName = NSProcessInfo.processInfo.environment[@"DARWIN_ART_PROFILE"] ?: @"default";
    _launchTasks = [NSMutableSet set];
    _launchRecords = [NSMapTable strongToStrongObjectsMapTable];
    return self;
}

- (NSURL *)discoverRuntimeRoot {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *override = NSProcessInfo.processInfo.environment[@"DARWIN_ART_RUNTIME_ROOT"];
    if (override.length > 0) {
        NSURL *candidate = [NSURL fileURLWithPath:override isDirectory:YES];
        if ([files isExecutableFileAtPath:
                       [candidate URLByAppendingPathComponent:@"target/release/darwin-artctl"].path]) {
            return candidate;
        }
    }
    NSURL *candidate = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"DarwinART"
                                                                         isDirectory:YES];
    BOOL hasControl = [files isExecutableFileAtPath:
                                 [candidate URLByAppendingPathComponent:
                                                @"target/release/darwin-artctl"].path];
    BOOL hasRuntime = [files fileExistsAtPath:
                                 [candidate URLByAppendingPathComponent:
                                                @"_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"].path];
    if (hasControl && hasRuntime) return candidate;
    return nil;
}

- (NSMutableDictionary<NSString *, NSString *> *)taskEnvironment {
    NSMutableDictionary *environment = [NSProcessInfo.processInfo.environment mutableCopy];
    environment[@"DARWIN_ART_PROFILE"] = self.profileName;
    environment[@"DARWIN_ART_PACKAGED_RUNTIME"] = @"1";
    environment[@"DARWIN_ART_ANGLE_DIRECTORY"] =
        [self.runtimeRootURL URLByAppendingPathComponent:
                                 @"_build/angle-source/out/DarwinArtRelease"].path;
    NSString *profileRoot = [NSHomeDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Library/Application Support/DarwinART/profiles/%@",
                                   self.profileName]];
    environment[@"DARWIN_ART_NATIVE_CACHE_ROOT"] =
        [profileRoot stringByAppendingPathComponent:@"native-cache"];
    return environment;
}

- (NSString *)runProgram:(NSString *)program
                arguments:(NSArray<NSString *> *)arguments
                     error:(NSError **)error {
    NSTask *task = [[NSTask alloc] init];
    NSPipe *output = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:program];
    task.arguments = arguments;
    task.environment = [self taskEnvironment];
    task.currentDirectoryURL = self.runtimeRootURL;
    task.standardOutput = output;
    task.standardError = output;
    if (![task launchAndReturnError:error]) return @"";
    NSData *data = [output.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    if (task.terminationStatus != 0) {
        if (error) {
            NSString *detail = [text stringByTrimmingCharactersInSet:
                                         NSCharacterSet.whitespaceAndNewlineCharacterSet];
            *error = [NSError errorWithDomain:DARErrorDomain
                                         code:task.terminationStatus
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    detail.length ? detail : @"명령 실행에 실패했습니다."}];
        }
        return @"";
    }
    return text;
}

- (void)fetchSnapshot:(DARRuntimeSnapshotHandler)handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                                  @"target/release/darwin-artctl"].path;
        [self runProgram:ctl arguments:@[@"ensure"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });
        NSString *packagesText = [self runProgram:ctl arguments:@[@"list"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });
        NSString *processText = [self runProgram:ctl arguments:@[@"ps"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });
        NSString *status = [self runProgram:ctl arguments:@[@"status"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });

        NSMutableArray<NSString *> *packages = [NSMutableArray array];
        [packagesText enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
            (void)stop;
            if (line.length) [packages addObject:line];
        }];
        NSMutableDictionary<NSString *, NSMutableArray<NSNumber *> *> *processes =
            [NSMutableDictionary dictionary];
        [processText enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
            (void)stop;
            NSArray<NSString *> *fields = [line componentsSeparatedByString:@"\t"];
            if (fields.count != 2) return;
            NSInteger pid = fields[0].integerValue;
            if (pid <= 0) return;
            NSMutableArray *values = processes[fields[1]];
            if (!values) processes[fields[1]] = values = [NSMutableArray array];
            [values addObject:@(pid)];
        }];
        DARRuntimeSnapshot *snapshot = [[DARRuntimeSnapshot alloc] init];
        snapshot.packages = packages;
        snapshot.processes = processes;
        snapshot.daemonStatus = [status stringByTrimmingCharactersInSet:
                                            NSCharacterSet.whitespaceAndNewlineCharacterSet];
        dispatch_async(dispatch_get_main_queue(), ^{ handler(snapshot, nil); });
    });
}

- (void)runActionProgram:(NSString *)program
                arguments:(NSArray<NSString *> *)arguments
                completion:(DARRuntimeActionHandler)handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *output = [self runProgram:program arguments:arguments error:&error];
        if (output.length && self.logHandler) {
            dispatch_async(dispatch_get_main_queue(), ^{ self.logHandler(output); });
        }
        dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
    });
}

- (void)installAPKAtURL:(NSURL *)url completion:(DARRuntimeActionHandler)handler {
    NSString *installer = [self.runtimeRootURL URLByAppendingPathComponent:
                                                     @"tools/run-android-apk-app.sh"].path;
    [self runActionProgram:@"/usr/bin/env"
                 arguments:@[@"DARWIN_ART_INSTALL_ONLY=1", installer, url.path, @"0"]
                completion:handler];
}

- (void)uninstallPackage:(NSString *)package
              removeData:(BOOL)removeData
              completion:(DARRuntimeActionHandler)handler {
    NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                              @"target/release/darwin-artctl"].path;
    NSArray<NSString *> *arguments = removeData
                                         ? @[@"uninstall", package]
                                         : @[@"uninstall", package, @"--keep-data"];
    [self runActionProgram:ctl arguments:arguments completion:handler];
}

- (void)launchPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler {
    NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                              @"target/release/darwin-artctl"].path;
    NSError *error = nil;
    NSString *record = [self runProgram:ctl arguments:@[@"resolve", package] error:&error];
    if (error) {
        handler(error);
        return;
    }
    NSURL *recordURL = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
        URLByAppendingPathComponent:[NSString stringWithFormat:@"darwin-art-launch-%@.record",
                                                               NSUUID.UUID.UUIDString]];
    if (![record writeToURL:recordURL atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
        handler(error);
        return;
    }
    NSString *launcher = [self.runtimeRootURL URLByAppendingPathComponent:
                                                   @"tools/run-android-apk-app.sh"].path;
    NSTask *task = [[NSTask alloc] init];
    NSString *profileRoot = [NSHomeDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Library/Application Support/DarwinART/profiles/%@",
                                   self.profileName]];
    if (![NSFileManager.defaultManager createDirectoryAtPath:profileRoot
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&error]) {
        handler(error);
        return;
    }
    NSString *logPath = [profileRoot stringByAppendingPathComponent:@"managed-apps.log"];
    if (![NSFileManager.defaultManager fileExistsAtPath:logPath]) {
        [NSFileManager.defaultManager createFileAtPath:logPath contents:nil attributes:nil];
    }
    NSFileHandle *log = [NSFileHandle fileHandleForWritingAtPath:logPath];
    if (!log) {
        handler([NSError errorWithDomain:DARErrorDomain
                                    code:4
                                userInfo:@{NSLocalizedDescriptionKey:
                                               @"프로필 로그 파일을 열지 못했습니다."}]);
        return;
    }
    [log seekToEndOfFile];
    task.executableURL = [NSURL fileURLWithPath:launcher];
    task.arguments = @[@"--record", recordURL.path, @"86400"];
    task.environment = [self taskEnvironment];
    task.currentDirectoryURL = self.runtimeRootURL;
    task.standardOutput = log;
    task.standardError = log;
    if (![task launchAndReturnError:&error]) {
        [log closeFile];
        [NSFileManager.defaultManager removeItemAtURL:recordURL error:nil];
        handler(error);
        return;
    }
    [log closeFile];
    [self.launchTasks addObject:task];
    [self.launchRecords setObject:recordURL forKey:task];
    __weak typeof(self) weakSelf = self;
    task.terminationHandler = ^(NSTask *finished) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSURL *finishedRecord = [weakSelf.launchRecords objectForKey:finished];
            if (finishedRecord) {
                [NSFileManager.defaultManager removeItemAtURL:finishedRecord error:nil];
                [weakSelf.launchRecords removeObjectForKey:finished];
            }
            [weakSelf.launchTasks removeObject:finished];
            if (weakSelf.logHandler) {
                weakSelf.logHandler([NSString stringWithFormat:@"%@ 종료 (status %d)\n",
                                                               package, finished.terminationStatus]);
            }
        });
    };
    handler(nil);
}

- (void)stopPackage:(NSString *)package
                pids:(NSArray<NSNumber *> *)pids
          completion:(DARRuntimeActionHandler)handler {
    if (pids.count == 0) {
        handler([NSError errorWithDomain:DARErrorDomain
                                    code:2
                                userInfo:@{NSLocalizedDescriptionKey: @"실행 중인 프로세스가 없습니다."}]);
        return;
    }
    NSMutableArray<NSString *> *arguments = [NSMutableArray arrayWithObject:@"-TERM"];
    for (NSNumber *pid in pids) [arguments addObject:pid.stringValue];
    if (self.logHandler) {
        NSString *pidList = [[pids valueForKey:@"stringValue"] componentsJoinedByString:@", "];
        self.logHandler([NSString stringWithFormat:@"%@에 정상 종료 요청 전달 (PID %@)\n",
                                                   package, pidList]);
    }
    [self runActionProgram:@"/bin/kill" arguments:arguments completion:handler];
}

- (void)revealDataForPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                                  @"target/release/darwin-artctl"].path;
        NSString *mount = [self runProgram:ctl arguments:@[@"ensure"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        mount = [mount stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString *path = [mount stringByAppendingPathComponent:
                                   [NSString stringWithFormat:@"data/apps/%@/private-data/user/0/%@",
                                                              package, package]];
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSWorkspace sharedWorkspace] selectFile:nil inFileViewerRootedAtPath:path];
            handler(nil);
        });
    });
}

- (void)restartSystem:(DARRuntimeActionHandler)handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                                  @"target/release/darwin-artctl"].path;
        NSString *processText = [self runProgram:ctl arguments:@[@"ps"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        NSMutableArray<NSString *> *pids = [NSMutableArray arrayWithObject:@"-TERM"];
        [processText enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
            (void)stop;
            NSString *pid = [[line componentsSeparatedByString:@"\t"] firstObject];
            if (pid.integerValue > 0) [pids addObject:pid];
        }];
        if (pids.count > 1) [self runProgram:@"/bin/kill" arguments:pids error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        for (NSUInteger attempt = 0; attempt < 50; attempt++) {
            [NSThread sleepForTimeInterval:0.1];
            NSString *remaining = [self runProgram:ctl arguments:@[@"ps"] error:&error];
            if (error || remaining.length == 0) break;
        }
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        [self runProgram:ctl arguments:@[@"shutdown"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        [self runProgram:ctl arguments:@[@"ensure"] error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
    });
}

- (void)revealProfile:(DARRuntimeActionHandler)handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *ctl = [self.runtimeRootURL URLByAppendingPathComponent:
                                                  @"target/release/darwin-artctl"].path;
        NSString *mount = [self runProgram:ctl arguments:@[@"ensure"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(error); });
        mount = [mount stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSWorkspace sharedWorkspace] selectFile:nil inFileViewerRootedAtPath:mount];
            handler(nil);
        });
    });
}

@end
