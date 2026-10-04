#include "tunnelalert.h"

#import <Cocoa/Cocoa.h>

void showTunnelStartupError(const QString& message)
{
    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Moonlight - Xray";
        alert.informativeText = message.toNSString();
        alert.alertStyle = NSAlertStyleCritical;
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }
}
