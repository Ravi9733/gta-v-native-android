#import "graphics_modes.h"
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <stdio.h>

void runtime_ios_set_output_height(int height);
void mfxup_set(int on);
void mfxup_output_set(int on);
void nvn_set_frame_rate_target(int fps);

NSNotificationName const GTAGraphicsModeDidChangeNotification = @"GTAGraphicsModeDidChange";
static NSString *const kModeKey = @"GTAImageOutputMode";
static NSString *const kFrameRateKey = @"GTAFrameRateLimit";

static NSArray<NSDictionary *> *modes(void) {
    static NSArray *all; static dispatch_once_t once;
    dispatch_once(&once, ^{
        all = @[
            @{ @"id": @"economy", @"title": @"Экономно", @"detail": @"Вывод 540p, мягче изображение.", @"height": @540 },
            @{ @"id": @"standard", @"title": @"Стандарт", @"detail": @"Вывод 720p, исходный вид.", @"height": @720 },
            @{ @"id": @"clear", @"title": @"Чётче", @"detail": @"Вывод до 1080p с MetalFX.", @"height": @1080 }
        ];
    });
    return all;
}

static BOOL metalFXSupported(void) {
    static BOOL supported; static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        supported = device && [MTLFXSpatialScalerDescriptor supportsMetal4FX:device];
    });
    return supported;
}

static NSDictionary *currentMode(void) {
    NSString *saved = [NSUserDefaults.standardUserDefaults stringForKey:kModeKey];
    for (NSDictionary *mode in modes())
        if ([mode[@"id"] isEqual:saved] && (![saved isEqual:@"clear"] || metalFXSupported())) return mode;
    return modes()[1];   // first launch or an invalid/unsupported saved value: original 720p output
}

static void applyMode(NSDictionary *mode) {
    BOOL clear = [mode[@"id"] isEqual:@"clear"];
    /* Only the generic final-output stage is used. TOTK-specific scene/docked replacements
       are not resolution controls for GTA; the game's own canvas stays 1280x720. */
    mfxup_output_set(clear);
    mfxup_set(clear);
    runtime_ios_set_output_height([mode[@"height"] intValue]);
    fprintf(stderr, "[gta-graphics] mode=%s game=1280x720 output_height=%d metalfx_output=%d\n",
            [mode[@"id"] UTF8String], [mode[@"height"] intValue], clear);
}

int GTAGraphicsCurrentFrameRate(void) {
    NSInteger saved = [NSUserDefaults.standardUserDefaults integerForKey:kFrameRateKey];
    return saved == 60 ? 60 : 30;
}
static void applyFrameRate(int fps) {
    nvn_set_frame_rate_target(fps);
    fprintf(stderr, "[gta-graphics] fps_target=%d (independent of output quality)\n", fps);
}
void GTAGraphicsApplySavedMode(void) { applyMode(currentMode()); applyFrameRate(GTAGraphicsCurrentFrameRate()); }
NSString *GTAGraphicsCurrentModeTitle(void) { return currentMode()[@"title"]; }

@interface GTAGraphicsMenu : UIView
- (instancetype)initWithClose:(void (^)(void))close;
@end

@implementation GTAGraphicsMenu {
    void (^onClose)(void);
    NSArray<UIButton *> *modeButtons;
    NSArray<UIButton *> *frameRateButtons;
}
- (instancetype)initWithClose:(void (^)(void))close {
    if ((self = [super initWithFrame:CGRectZero])) {
        onClose = [close copy];
        self.backgroundColor = [UIColor colorWithWhite:0 alpha:.48];
        self.accessibilityViewIsModal = YES;
        UIButton *backdrop = [UIButton buttonWithType:UIButtonTypeCustom];
        backdrop.frame = self.bounds; backdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        backdrop.accessibilityElementsHidden = YES;
        [backdrop addTarget:self action:@selector(closeMenu) forControlEvents:UIControlEventTouchUpInside];
        [self addSubview:backdrop];

        UIView *panel = [UIView new]; panel.translatesAutoresizingMaskIntoConstraints = NO;
        panel.backgroundColor = [UIColor colorWithWhite:.12 alpha:.98]; panel.layer.cornerRadius = 18;
        panel.layer.borderWidth = 1; panel.layer.borderColor = [UIColor colorWithWhite:1 alpha:.22].CGColor;
        [self addSubview:panel];
        UILayoutGuide *safe = self.safeAreaLayoutGuide;
        NSLayoutConstraint *width = [panel.widthAnchor constraintEqualToConstant:500]; width.priority = 999;
        NSLayoutConstraint *height = [panel.heightAnchor constraintEqualToConstant:350]; height.priority = 999;
        [NSLayoutConstraint activateConstraints:@[
            [panel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor], [panel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
            [panel.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-24],
            [panel.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-20], width, height
        ]];
        UILabel *title = [UILabel new]; title.text = @"Графика"; title.textColor = UIColor.whiteColor;
        title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; title.adjustsFontForContentSizeCategory = YES;
        title.accessibilityTraits |= UIAccessibilityTraitHeader;
        UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem]; [done setTitle:@"Готово" forState:UIControlStateNormal];
        [done setTitleColor:UIColor.whiteColor forState:UIControlStateNormal]; done.accessibilityLabel = @"Закрыть настройки графики";
        [done addTarget:self action:@selector(closeMenu) forControlEvents:UIControlEventTouchUpInside];
        [done.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;
        [done.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
        UIStackView *header = [[UIStackView alloc] initWithArrangedSubviews:@[title, done]];
        header.axis = UILayoutConstraintAxisHorizontal; header.alignment = UIStackViewAlignmentCenter;
        header.translatesAutoresizingMaskIntoConstraints = NO; [panel addSubview:header];

        UIScrollView *scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO; [panel addSubview:scroll];
        UIStackView *content = [UIStackView new]; content.axis = UILayoutConstraintAxisVertical; content.spacing = 10;
        content.translatesAutoresizingMaskIntoConstraints = NO; [scroll addSubview:content];
        [NSLayoutConstraint activateConstraints:@[
            [header.topAnchor constraintEqualToAnchor:panel.topAnchor constant:6], [header.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor constant:18],
            [header.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor constant:-8],
            [scroll.topAnchor constraintEqualToAnchor:header.bottomAnchor constant:4], [scroll.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor constant:-12],
            [scroll.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor constant:12], [scroll.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor constant:-12],
            [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor], [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
            [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor], [content.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
            [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor]
        ]];
        UILabel *subtitle = [UILabel new]; subtitle.numberOfLines = 0;
        subtitle.text = @"Игра: 720p. Режим «Чётче» улучшает масштабирование для экрана.";
        subtitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote]; subtitle.adjustsFontForContentSizeCategory = YES;
        subtitle.textColor = [UIColor colorWithWhite:1 alpha:.78]; [content addArrangedSubview:subtitle];
        NSMutableArray *buttons = [NSMutableArray array];
        for (NSUInteger i = 0; i < modes().count; i++) {
            UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem]; button.tag = (NSInteger)i;
            [button.heightAnchor constraintGreaterThanOrEqualToConstant:62].active = YES;
            [button addTarget:self action:@selector(selectMode:) forControlEvents:UIControlEventTouchUpInside];
            [content addArrangedSubview:button]; [buttons addObject:button];
        }
        modeButtons = buttons;
        UILabel *frameRateTitle = [UILabel new]; frameRateTitle.text = @"Частота кадров";
        frameRateTitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        frameRateTitle.adjustsFontForContentSizeCategory = YES; frameRateTitle.textColor = UIColor.whiteColor;
        frameRateTitle.accessibilityTraits |= UIAccessibilityTraitHeader; [content addArrangedSubview:frameRateTitle];
        UIStackView *frameRateRow = [UIStackView new]; frameRateRow.axis = UILayoutConstraintAxisHorizontal;
        frameRateRow.spacing = 10; frameRateRow.distribution = UIStackViewDistributionFillEqually;
        NSMutableArray *fpsButtons = [NSMutableArray array];
        for (NSNumber *fps in @[@30, @60]) {
            UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem]; button.tag = fps.integerValue;
            [button.heightAnchor constraintGreaterThanOrEqualToConstant:54].active = YES;
            [button addTarget:self action:@selector(selectFrameRate:) forControlEvents:UIControlEventTouchUpInside];
            [frameRateRow addArrangedSubview:button]; [fpsButtons addObject:button];
        }
        frameRateButtons = fpsButtons; [content addArrangedSubview:frameRateRow];
        UILabel *fpsNote = [UILabel new]; fpsNote.text = @"60 FPS — тестовый режим; частота зависит от сцены."; fpsNote.numberOfLines = 0;
        fpsNote.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; fpsNote.adjustsFontForContentSizeCategory = YES;
        fpsNote.textColor = [UIColor colorWithWhite:1 alpha:.65]; [content addArrangedSubview:fpsNote];
        UILabel *note = [UILabel new]; note.text = @"Переключается сразу. Выбор сохранится."; note.numberOfLines = 0;
        note.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; note.adjustsFontForContentSizeCategory = YES;
        note.textColor = [UIColor colorWithWhite:1 alpha:.65]; [content addArrangedSubview:note];
        [self refreshModes];
        [self refreshFrameRate];
    }
    return self;
}
- (void)refreshModes {
    NSString *selected = currentMode()[@"id"];
    for (NSUInteger i = 0; i < modeButtons.count; i++) {
        UIButton *button = modeButtons[i]; NSDictionary *mode = modes()[i];
        BOOL chosen = [mode[@"id"] isEqual:selected], supported = ![mode[@"id"] isEqual:@"clear"] || metalFXSupported();
        UIButtonConfiguration *configuration = [UIButtonConfiguration grayButtonConfiguration];
        configuration.title = mode[@"title"]; configuration.subtitle = supported ? mode[@"detail"] : @"MetalFX недоступен на этом устройстве.";
        configuration.titleAlignment = UIButtonConfigurationTitleAlignmentLeading;
        configuration.baseForegroundColor = UIColor.whiteColor;
        configuration.background.backgroundColor = [UIColor colorWithWhite:chosen ? .34 : .22 alpha:1];
        configuration.background.cornerRadius = 12;
        configuration.contentInsets = NSDirectionalEdgeInsetsMake(10, 14, 10, 14);
        configuration.image = chosen ? [UIImage systemImageNamed:@"checkmark.circle.fill"] : nil;
        configuration.imagePlacement = NSDirectionalRectEdgeTrailing; configuration.imagePadding = 10;
        button.configuration = configuration; button.enabled = supported;
        button.accessibilityLabel = [NSString stringWithFormat:@"%@. %@", mode[@"title"], configuration.subtitle];
        button.accessibilityValue = chosen ? @"Выбран" : @"";
        button.accessibilityTraits = UIAccessibilityTraitButton | (chosen ? UIAccessibilityTraitSelected : 0);
    }
}
- (void)selectMode:(UIButton *)button {
    NSDictionary *mode = modes()[(NSUInteger)button.tag];
    if ([mode[@"id"] isEqual:@"clear"] && !metalFXSupported()) return;
    [NSUserDefaults.standardUserDefaults setObject:mode[@"id"] forKey:kModeKey];
    applyMode(mode); [self refreshModes];
    [NSNotificationCenter.defaultCenter postNotificationName:GTAGraphicsModeDidChangeNotification object:nil];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, [NSString stringWithFormat:@"%@, выбран", mode[@"title"]]);
}
- (void)refreshFrameRate {
    int selected = GTAGraphicsCurrentFrameRate();
    for (UIButton *button in frameRateButtons) {
        BOOL chosen = button.tag == selected;
        UIButtonConfiguration *configuration = [UIButtonConfiguration grayButtonConfiguration];
        configuration.title = [NSString stringWithFormat:@"%ld FPS", (long)button.tag];
        configuration.baseForegroundColor = UIColor.whiteColor;
        configuration.background.backgroundColor = [UIColor colorWithWhite:chosen ? .34 : .22 alpha:1];
        configuration.background.cornerRadius = 12;
        configuration.contentInsets = NSDirectionalEdgeInsetsMake(10, 14, 10, 14);
        configuration.image = chosen ? [UIImage systemImageNamed:@"checkmark.circle.fill"] : nil;
        configuration.imagePlacement = NSDirectionalRectEdgeTrailing; configuration.imagePadding = 10;
        button.configuration = configuration; button.accessibilityLabel = configuration.title;
        button.accessibilityValue = chosen ? @"Выбран" : @"";
        button.accessibilityTraits = UIAccessibilityTraitButton | (chosen ? UIAccessibilityTraitSelected : 0);
    }
}
- (void)selectFrameRate:(UIButton *)button {
    int fps = (int)button.tag; if (fps != 30 && fps != 60) return;
    [NSUserDefaults.standardUserDefaults setInteger:fps forKey:kFrameRateKey];
    applyFrameRate(fps); [self refreshFrameRate];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, [NSString stringWithFormat:@"Цель %d FPS, выбрана", fps]);
}
- (void)closeMenu { if (onClose) onClose(); }
- (BOOL)accessibilityPerformEscape { [self closeMenu]; return YES; }
@end

UIView *GTACreateGraphicsMenu(void (^onClose)(void)) { return [[GTAGraphicsMenu alloc] initWithClose:onClose]; }
