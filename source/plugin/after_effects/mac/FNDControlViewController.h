
#ifndef _FNDControlViewController_h_
#define _FNDControlViewController_h_

#import <Cocoa/Cocoa.h>
#import "FNDObjC.h"

@class FND_OBJC(ControlViewController);
@compatibility_alias FNDControlViewController FND_OBJC(ControlViewController);

@protocol FNDControlDelegate <NSObject>
@required
- (void)controlValueDidChange:(FNDControlViewController *)control;
@end

@interface FNDControlViewController : NSViewController
@property int controlIdentifier;
@property double doubleValue;
@property NSInteger selectedTag;
@property BOOL enabled;
@property BOOL useAuto;
@property (weak) id <FNDControlDelegate> delegate;
@end

#endif
