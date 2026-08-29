#import "DARRuntimeClient.h"

#import <AppKit/AppKit.h>

static NSString *const DARErrorDomain = @"dev.darwinart.manager";

@implementation DARRuntimeSnapshot
@end

@interface DARRuntimeClient ()
@property(nonatomic, readwrite) NSURL *projectRootURL;
@property(nonatomic, readwrite) NSString *profileName;
@property(nonatomic) NSMutableSet<NSTask *> *launchTasks;
@end

@implementation DARRuntimeClient

- (nullable instancetype)initWithError:(NSError **)error {
    self = [super init];
    if (!self) return nil;

    NSURL *root = [self discoverProjectRoot];
    if (!root) {
        if (error) {
            *error = [NSError errorWithDomain:DARErrorDomain
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"Darwin ART 프로젝트를 찾지 못했습니다. "
                                                     "DARWIN_ART_ROOT를 설정해 주세요."}];
        }
        return nil;
    }
    _projectRootURL = root;
    _profileName = NSProcessInfo.processInfo.environment[@"DARWIN_ART_PROFILE"] ?: @"default";
    _launchTasks = [NSMutableSet set];
    return self;
}

- (NSURL *)discoverProjectRoot {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *override = NSProcessInfo.processInfo.environment[@"DARWIN_ART_ROOT"];
    if (override.length > 0) {
        NSURL *candidate = [NSURL fileURLWithPath:override isDirectory:YES];
        if ([files isExecutableFileAtPath:
                       [candidate URLByAppendingPathComponent:@"tools/darwin-art"].path]) {
            return candidate;
        }
    }

    NSURL *bundleParent = NSBundle.mainBundle.bundleURL.URLByDeletingLastPathComponent;
    if ([bundleParent.lastPathComponent isEqualToString:@"_build"]) {
        NSURL *candidate = bundleParent.URLByDeletingLastPathComponent;
        if ([files isExecutableFileAtPath:
                       [candidate URLByAppendingPathComponent:@"tools/darwin-art"].path]) {
            return candidate;
        }
    }

    NSURL *candidate = [NSURL fileURLWithPath:NSFileManager.defaultManager.currentDirectoryPath
                                   isDirectory:YES];
    for (NSUInteger depth = 0; depth < 8; depth++) {
        if ([files isExecutableFileAtPath:
                       [candidate URLByAppendingPathComponent:@"tools/darwin-art"].path]) {
            return candidate;
        }
        NSURL *parent = candidate.URLByDeletingLastPathComponent;
        if ([parent.path isEqualToString:candidate.path]) break;
        candidate = parent;
    }
    return nil;
}

- (NSMutableDictionary<NSString *, NSString *> *)taskEnvironment {
    NSMutableDictionary *environment = [NSProcessInfo.processInfo.environment mutableCopy];
    environment[@"DARWIN_ART_PROFILE"] = self.profileName;
    environment[@"DARWIN_ART_ROOT"] = self.projectRootURL.path;
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
    task.currentDirectoryURL = self.projectRootURL;
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
        NSString *tool = [self.projectRootURL URLByAppendingPathComponent:@"tools/darwin-art"].path;
        NSString *packagesText = [self runProgram:tool arguments:@[@"list"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });
        NSString *processText = [self runProgram:tool arguments:@[@"ps"] error:&error];
        if (error) return dispatch_async(dispatch_get_main_queue(), ^{ handler(nil, error); });
        NSString *ctl = [self.projectRootURL URLByAppendingPathComponent:
                                                  @"target/release/darwin-artctl"].path;
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
    NSString *tool = [self.projectRootURL URLByAppendingPathComponent:@"tools/darwin-art"].path;
    [self runActionProgram:tool arguments:@[@"install", url.path] completion:handler];
}

- (void)launchPackage:(NSString *)package completion:(DARRuntimeActionHandler)handler {
    NSString *tool = [self.projectRootURL URLByAppendingPathComponent:@"tools/darwin-art"].path;
    NSTask *task = [[NSTask alloc] init];
    NSString *profileRoot = [NSHomeDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Library/Application Support/DarwinART/profiles/%@",
                                   self.profileName]];
    NSError *error = nil;
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
    task.executableURL = [NSURL fileURLWithPath:tool];
    task.arguments = @[@"run", package, @"86400"];
    task.environment = [self taskEnvironment];
    task.currentDirectoryURL = self.projectRootURL;
    task.standardOutput = log;
    task.standardError = log;
    if (![task launchAndReturnError:&error]) {
        [log closeFile];
        handler(error);
        return;
    }
    [log closeFile];
    [self.launchTasks addObject:task];
    __weak typeof(self) weakSelf = self;
    task.terminationHandler = ^(NSTask *finished) {
        dispatch_async(dispatch_get_main_queue(), ^{
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
        NSString *ctl = [self.projectRootURL URLByAppendingPathComponent:
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

@end
