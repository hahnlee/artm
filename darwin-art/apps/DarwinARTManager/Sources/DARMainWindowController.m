#import "DARMainWindowController.h"

#import "DARRuntimeClient.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static NSToolbarItemIdentifier const DARInstallItem = @"dev.darwinart.manager.install";
static NSToolbarItemIdentifier const DARRefreshItem = @"dev.darwinart.manager.refresh";

@interface DARMainWindowController () <NSTableViewDataSource, NSTableViewDelegate,
                                        NSToolbarDelegate>
@property(nonatomic) DARRuntimeClient *client;
@property(nonatomic) DARRuntimeSnapshot *snapshot;
@property(nonatomic) NSArray<NSString *> *packages;
@property(nonatomic) NSString *selectedPackage;
@property(nonatomic) NSTableView *tableView;
@property(nonatomic) NSTextField *packageLabel;
@property(nonatomic) NSTextField *stateLabel;
@property(nonatomic) NSTextField *daemonLabel;
@property(nonatomic) NSButton *runButton;
@property(nonatomic) NSButton *stopButton;
@property(nonatomic) NSButton *dataButton;
@property(nonatomic) NSTextView *logView;
@property(nonatomic) NSProgressIndicator *progress;
@property(nonatomic) NSMutableString *pendingLogText;
@end

@implementation DARMainWindowController

- (instancetype)initWithRuntimeClient:(DARRuntimeClient *)client {
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 940, 620)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self = [super initWithWindow:window];
    if (!self) return nil;
    _client = client;
    _packages = @[];
    _pendingLogText = [NSMutableString string];
    window.title = @"Darwin ART";
    window.subtitle = [NSString stringWithFormat:@"%@ 프로필", client.profileName];
    window.minSize = NSMakeSize(760, 500);
    window.titlebarAppearsTransparent = YES;
    window.toolbarStyle = NSWindowToolbarStyleUnified;
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    [window center];
    [self buildInterface];
    [self configureToolbar];
    __weak typeof(self) weakSelf = self;
    client.logHandler = ^(NSString *text) { [weakSelf appendLog:text]; };
    return self;
}

- (NSTextField *)labelWithFont:(NSFont *)font color:(NSColor *)color {
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    return label;
}

- (void)buildInterface {
    NSSplitView *split = [[NSSplitView alloc] init];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.translatesAutoresizingMaskIntoConstraints = NO;
    self.window.contentView = split;

    NSVisualEffectView *sidebar = [[NSVisualEffectView alloc] init];
    sidebar.material = NSVisualEffectMaterialSidebar;
    sidebar.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    sidebar.state = NSVisualEffectStateFollowsWindowActiveState;
    sidebar.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView *listScroll = [[NSScrollView alloc] init];
    listScroll.drawsBackground = NO;
    listScroll.hasVerticalScroller = YES;
    listScroll.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView = [[NSTableView alloc] init];
    self.tableView.headerView = nil;
    self.tableView.rowHeight = 36;
    self.tableView.backgroundColor = NSColor.clearColor;
    self.tableView.style = NSTableViewStyleSourceList;
    self.tableView.delegate = self;
    self.tableView.dataSource = self;
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"package"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [self.tableView addTableColumn:column];
    listScroll.documentView = self.tableView;
    [sidebar addSubview:listScroll];
    [NSLayoutConstraint activateConstraints:@[
        [listScroll.leadingAnchor constraintEqualToAnchor:sidebar.leadingAnchor],
        [listScroll.trailingAnchor constraintEqualToAnchor:sidebar.trailingAnchor],
        [listScroll.topAnchor constraintEqualToAnchor:sidebar.topAnchor],
        [listScroll.bottomAnchor constraintEqualToAnchor:sidebar.bottomAnchor],
        [sidebar.widthAnchor constraintEqualToConstant:280],
    ]];

    NSView *detail = [[NSView alloc] init];
    detail.translatesAutoresizingMaskIntoConstraints = NO;
    NSImageView *icon = [[NSImageView alloc] init];
    icon.image = [NSImage imageWithSystemSymbolName:@"apps.iphone"
                           accessibilityDescription:@"Android applications"];
    icon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:38
                                                                                weight:NSFontWeightRegular];
    icon.contentTintColor = NSColor.controlAccentColor;
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    self.packageLabel = [self labelWithFont:[NSFont systemFontOfSize:24 weight:NSFontWeightSemibold]
                                      color:NSColor.labelColor];
    self.packageLabel.stringValue = @"앱을 선택하세요";
    self.stateLabel = [self labelWithFont:[NSFont systemFontOfSize:13]
                                    color:NSColor.secondaryLabelColor];
    self.stateLabel.stringValue = @"설치된 Android 앱을 관리할 수 있습니다.";

    self.runButton = [NSButton buttonWithTitle:@"실행" target:self action:@selector(runSelected:)];
    self.runButton.bezelStyle = NSBezelStyleRounded;
    self.runButton.keyEquivalent = @"\r";
    self.stopButton = [NSButton buttonWithTitle:@"종료" target:self action:@selector(stopSelected:)];
    self.stopButton.bezelStyle = NSBezelStyleRounded;
    self.dataButton = [NSButton buttonWithTitle:@"앱 데이터 보기"
                                         target:self
                                         action:@selector(revealSelected:)];
    self.dataButton.bezelStyle = NSBezelStyleRounded;
    NSStackView *actions = [NSStackView stackViewWithViews:
                                      @[self.runButton, self.stopButton, self.dataButton]];
    actions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    actions.spacing = 8;
    actions.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *logTitle = [self labelWithFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]
                                         color:NSColor.labelColor];
    logTitle.stringValue = @"런타임 로그";
    NSScrollView *logScroll = [[NSScrollView alloc] init];
    logScroll.borderType = NSBezelBorder;
    logScroll.hasVerticalScroller = YES;
    logScroll.translatesAutoresizingMaskIntoConstraints = NO;
    self.logView = [[NSTextView alloc] init];
    self.logView.editable = NO;
    self.logView.selectable = YES;
    self.logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.logView.textColor = NSColor.secondaryLabelColor;
    self.logView.backgroundColor = NSColor.textBackgroundColor;
    self.logView.textContainerInset = NSMakeSize(8, 8);
    logScroll.documentView = self.logView;

    self.daemonLabel = [self labelWithFont:[NSFont systemFontOfSize:11]
                                     color:NSColor.tertiaryLabelColor];
    self.daemonLabel.stringValue = @"daemon 상태 확인 중…";
    self.progress = [[NSProgressIndicator alloc] init];
    self.progress.style = NSProgressIndicatorStyleSpinning;
    self.progress.controlSize = NSControlSizeSmall;
    self.progress.displayedWhenStopped = NO;
    self.progress.translatesAutoresizingMaskIntoConstraints = NO;

    for (NSView *view in @[icon, self.packageLabel, self.stateLabel, actions, logTitle,
                            logScroll, self.daemonLabel, self.progress]) {
        [detail addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:detail.leadingAnchor constant:32],
        [icon.topAnchor constraintEqualToAnchor:detail.topAnchor constant:36],
        [icon.widthAnchor constraintEqualToConstant:52],
        [icon.heightAnchor constraintEqualToConstant:52],
        [self.packageLabel.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:16],
        [self.packageLabel.trailingAnchor constraintLessThanOrEqualToAnchor:detail.trailingAnchor constant:-24],
        [self.packageLabel.topAnchor constraintEqualToAnchor:icon.topAnchor constant:2],
        [self.stateLabel.leadingAnchor constraintEqualToAnchor:self.packageLabel.leadingAnchor],
        [self.stateLabel.trailingAnchor constraintEqualToAnchor:detail.trailingAnchor constant:-24],
        [self.stateLabel.topAnchor constraintEqualToAnchor:self.packageLabel.bottomAnchor constant:4],
        [actions.leadingAnchor constraintEqualToAnchor:detail.leadingAnchor constant:32],
        [actions.topAnchor constraintEqualToAnchor:icon.bottomAnchor constant:24],
        [logTitle.leadingAnchor constraintEqualToAnchor:detail.leadingAnchor constant:32],
        [logTitle.topAnchor constraintEqualToAnchor:actions.bottomAnchor constant:28],
        [logScroll.leadingAnchor constraintEqualToAnchor:detail.leadingAnchor constant:32],
        [logScroll.trailingAnchor constraintEqualToAnchor:detail.trailingAnchor constant:-32],
        [logScroll.topAnchor constraintEqualToAnchor:logTitle.bottomAnchor constant:8],
        [logScroll.bottomAnchor constraintEqualToAnchor:self.daemonLabel.topAnchor constant:-14],
        [self.daemonLabel.leadingAnchor constraintEqualToAnchor:detail.leadingAnchor constant:32],
        [self.daemonLabel.bottomAnchor constraintEqualToAnchor:detail.bottomAnchor constant:-18],
        [self.progress.leadingAnchor constraintEqualToAnchor:self.daemonLabel.trailingAnchor constant:8],
        [self.progress.centerYAnchor constraintEqualToAnchor:self.daemonLabel.centerYAnchor],
    ]];

    [split addArrangedSubview:sidebar];
    [split addArrangedSubview:detail];
    [split setPosition:280 ofDividerAtIndex:0];
    [self updateControls];
}

- (void)configureToolbar {
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"DarwinARTToolbar"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    self.window.toolbar = toolbar;
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    return @[DARInstallItem, DARRefreshItem, NSToolbarFlexibleSpaceItemIdentifier];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
    return @[DARInstallItem, DARRefreshItem, NSToolbarFlexibleSpaceItemIdentifier];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
       itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
   willBeInsertedIntoToolbar:(BOOL)flag {
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
    if ([identifier isEqualToString:DARInstallItem]) {
        item.label = @"APK 설치";
        item.toolTip = @"APK 파일 설치";
        item.image = [NSImage imageWithSystemSymbolName:@"plus.app"
                               accessibilityDescription:item.label];
        item.target = self;
        item.action = @selector(installAPK:);
    } else {
        item.label = @"새로 고침";
        item.toolTip = @"앱과 프로세스 상태 새로 고침";
        item.image = [NSImage imageWithSystemSymbolName:@"arrow.clockwise"
                               accessibilityDescription:item.label];
        item.target = self;
        item.action = @selector(refresh:);
    }
    return item;
}

- (void)showWindow:(id)sender {
    [super showWindow:sender];
    [self refresh:nil];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    return self.packages.count;
}

- (NSView *)tableView:(NSTableView *)tableView
    viewForTableColumn:(NSTableColumn *)tableColumn
                   row:(NSInteger)row {
    NSTableCellView *cell = [tableView makeViewWithIdentifier:@"PackageCell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"PackageCell";
        NSImageView *image = [[NSImageView alloc] init];
        image.image = [NSImage imageWithSystemSymbolName:@"app"
                                 accessibilityDescription:@"Android app"];
        image.contentTintColor = NSColor.controlAccentColor;
        image.translatesAutoresizingMaskIntoConstraints = NO;
        NSTextField *text = [NSTextField labelWithString:@""];
        text.font = [NSFont systemFontOfSize:13];
        text.lineBreakMode = NSLineBreakByTruncatingMiddle;
        text.translatesAutoresizingMaskIntoConstraints = NO;
        cell.imageView = image;
        cell.textField = text;
        [cell addSubview:image];
        [cell addSubview:text];
        [NSLayoutConstraint activateConstraints:@[
            [image.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
            [image.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [image.widthAnchor constraintEqualToConstant:20],
            [image.heightAnchor constraintEqualToConstant:20],
            [text.leadingAnchor constraintEqualToAnchor:image.trailingAnchor constant:8],
            [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
            [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    NSString *package = self.packages[row];
    cell.textField.stringValue = package;
    cell.imageView.contentTintColor = self.snapshot.processes[package].count
                                          ? NSColor.systemGreenColor
                                          : NSColor.controlAccentColor;
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    NSInteger row = self.tableView.selectedRow;
    self.selectedPackage = row >= 0 && row < (NSInteger)self.packages.count
                               ? self.packages[row]
                               : nil;
    [self updateControls];
}

- (void)setBusy:(BOOL)busy {
    if (busy) [self.progress startAnimation:nil]; else [self.progress stopAnimation:nil];
}

- (void)updateControls {
    NSString *package = self.selectedPackage;
    NSArray<NSNumber *> *pids = package ? self.snapshot.processes[package] : @[];
    self.packageLabel.stringValue = package ?: @"앱을 선택하세요";
    if (!package) {
        self.stateLabel.stringValue = @"설치된 Android 앱을 관리할 수 있습니다.";
    } else if (pids.count) {
        NSString *pidList = [[pids valueForKey:@"stringValue"] componentsJoinedByString:@", "];
        self.stateLabel.stringValue = [NSString stringWithFormat:@"실행 중 · PID %@",
                                                                 pidList];
    } else {
        self.stateLabel.stringValue = @"설치됨 · 실행 중이 아님";
    }
    self.runButton.enabled = package.length > 0 && pids.count == 0;
    self.stopButton.enabled = package.length > 0 && pids.count > 0;
    self.dataButton.enabled = package.length > 0;
}

- (void)appendLog:(NSString *)text {
    if (!text.length) return;
    [self.pendingLogText appendString:text];
    while (YES) {
        NSRange newline = [self.pendingLogText rangeOfString:@"\n"];
        if (newline.location == NSNotFound) break;
        NSString *message = [self.pendingLogText substringToIndex:newline.location];
        [self.pendingLogText deleteCharactersInRange:NSMakeRange(0, NSMaxRange(newline))];
        NSString *line = [NSString stringWithFormat:@"[%@] %@\n",
                           [NSDateFormatter localizedStringFromDate:NSDate.date
                                                           dateStyle:NSDateFormatterNoStyle
                                                           timeStyle:NSDateFormatterMediumStyle],
                           message];
        [self.logView.textStorage appendAttributedString:
             [[NSAttributedString alloc] initWithString:line]];
    }
    if (self.logView.string.length > 200000) {
        NSRange boundary = [self.logView.string rangeOfString:@"\n"
                                                      options:0
                                                        range:NSMakeRange(50000, self.logView.string.length - 50000)];
        NSUInteger length = boundary.location == NSNotFound ? 50000 : NSMaxRange(boundary);
        [self.logView.textStorage deleteCharactersInRange:NSMakeRange(0, length)];
    }
    [self.logView scrollRangeToVisible:NSMakeRange(self.logView.string.length, 0)];
}

- (void)presentActionError:(NSError *)error {
    [self appendLog:[NSString stringWithFormat:@"오류: %@\n", error.localizedDescription]];
    NSAlert *alert = [NSAlert alertWithError:error];
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (IBAction)refresh:(id)sender {
    [self setBusy:YES];
    __weak typeof(self) weakSelf = self;
    [self.client fetchSnapshot:^(DARRuntimeSnapshot *snapshot, NSError *error) {
        [weakSelf setBusy:NO];
        if (error) {
            [weakSelf presentActionError:error];
            return;
        }
        weakSelf.snapshot = snapshot;
        weakSelf.packages = snapshot.packages;
        weakSelf.daemonLabel.stringValue = [NSString stringWithFormat:@"darwin-artd: %@ · profile: %@",
                                                                       snapshot.daemonStatus,
                                                                       weakSelf.client.profileName];
        [weakSelf.tableView reloadData];
        NSInteger row = [weakSelf.packages indexOfObject:weakSelf.selectedPackage];
        if (row != NSNotFound) {
            [weakSelf.tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                            byExtendingSelection:NO];
        } else if (weakSelf.packages.count) {
            [weakSelf.tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                            byExtendingSelection:NO];
        }
        [weakSelf updateControls];
    }];
}

- (IBAction)installAPK:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Android APK 설치";
    panel.prompt = @"설치";
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"apk"]];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) return;
        [self setBusy:YES];
        [self.client installAPKAtURL:panel.URL completion:^(NSError *error) {
            [self setBusy:NO];
            if (error) {
                [self presentActionError:error];
                return;
            }
            [self appendLog:[NSString stringWithFormat:@"%@ 설치 완료\n", panel.URL.lastPathComponent]];
            [self refresh:nil];
        }];
    }];
}

- (IBAction)runSelected:(id)sender {
    NSString *package = self.selectedPackage;
    if (!package) return;
    [self appendLog:[NSString stringWithFormat:@"%@ 실행 요청\n", package]];
    [self.client launchPackage:package completion:^(NSError *error) {
        if (error) {
            [self presentActionError:error];
            return;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [self refresh:nil]; });
    }];
}

- (IBAction)stopSelected:(id)sender {
    NSString *package = self.selectedPackage;
    NSArray<NSNumber *> *pids = self.snapshot.processes[package] ?: @[];
    if (!package) return;
    [self.client stopPackage:package pids:pids completion:^(NSError *error) {
        if (error) {
            [self presentActionError:error];
            return;
        }
        [self appendLog:[NSString stringWithFormat:@"%@ 종료 요청\n", package]];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [self refresh:nil]; });
    }];
}

- (IBAction)revealSelected:(id)sender {
    NSString *package = self.selectedPackage;
    if (!package) return;
    [self.client revealDataForPackage:package completion:^(NSError *error) {
        if (error) [self presentActionError:error];
    }];
}

@end
