#import <UIKit/UIKit.h>

FOUNDATION_EXPORT NSNotificationName const GTAGraphicsModeDidChangeNotification;

/* Independent saved output and FPS choices. The game's 1280x720 render targets are unchanged. */
void GTAGraphicsApplySavedMode(void);
NSString *GTAGraphicsCurrentModeTitle(void);
int GTAGraphicsCurrentFrameRate(void);
UIView *GTACreateGraphicsMenu(void (^onClose)(void));
