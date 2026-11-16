// pajupilli as an Audio Unit v3 instrument (aumu Pjpl pjpl).
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

@interface PajupilliAU : AUAudioUnit
@end

// The extension's principal class: it makes the Audio Unit for the host.
@interface PajupilliAUFactory : NSObject <AUAudioUnitFactory>
@end
