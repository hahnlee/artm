#import <AppKit/AppKit.h>
#import "DARAppDelegate.h"

int main(void) {
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        DARAppDelegate *delegate = [[DARAppDelegate alloc] init];
        application.delegate = delegate;
        [application run];
    }
    return 0;
}
