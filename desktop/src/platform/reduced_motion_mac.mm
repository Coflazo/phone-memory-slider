#import <AppKit/AppKit.h>

namespace pms::desktop::detail {

// Accessibility > Display > Reduce motion, as AppKit reports it.
bool mac_reduced_motion() noexcept {
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion] == YES;
    }
}

}  // namespace pms::desktop::detail
