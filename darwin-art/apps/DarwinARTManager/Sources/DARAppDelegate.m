#import "DARAppDelegate.h"

#import "DARMainWindowController.h"
#import "DARRuntimeClient.h"

@interface DARAppDelegate ()
@property(nonatomic) DARMainWindowController *mainWindowController;
@end

@implementation DARAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self installApplicationIcon];
    NSError *error = nil;
    DARRuntimeClient *client = [[DARRuntimeClient alloc] initWithError:&error];
    if (!client) {
        [[NSAlert alertWithError:error] runModal];
        [NSApp terminate:nil];
        return;
    }
    self.mainWindowController = [[DARMainWindowController alloc] initWithRuntimeClient:client];
    [self.mainWindowController showWindow:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (void)installApplicationIcon {
    NSImage *icon = [[NSImage alloc] initWithSize:NSMakeSize(256, 256)];
    [icon lockFocus];
    NSBezierPath *background = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(16, 16, 224, 224)
                                                               xRadius:50
                                                               yRadius:50];
    [[NSColor colorWithCalibratedRed:0.20 green:0.42 blue:0.96 alpha:1.0] setFill];
    [background fill];
    NSImage *symbol = [NSImage imageWithSystemSymbolName:@"apps.iphone.fill"
                                accessibilityDescription:@"Darwin ART"];
    symbol = [symbol imageWithSymbolConfiguration:
                         [NSImageSymbolConfiguration configurationWithPointSize:118
                                                                         weight:NSFontWeightMedium]];
    symbol.template = YES;
    [NSColor.whiteColor set];
    [symbol drawInRect:NSMakeRect(60, 60, 136, 136)
              fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver
              fraction:1.0];
    [icon unlockFocus];
    NSApp.applicationIconImage = icon;
}

@end
