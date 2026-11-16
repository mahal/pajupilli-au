// The host app: it carries the Audio Unit extension. Open it once after
// building and macOS registers the instrument; Logic finds it under
// AU Instruments > Pajupilli.
import SwiftUI

@main
struct PajupilliApp: App {
    var body: some Scene {
        WindowGroup {
            VStack(alignment: .leading, spacing: 12) {
                Text("Pajupilli").font(.largeTitle.bold())
                Text("A physical model of winds, brass, pianos and more, as an Audio Unit instrument.")
                Text("In Logic Pro: a new Software Instrument track, then AU Instruments › Werckmeister › Pajupilli. The parameters are under Controls in the plug-in window.")
                    .foregroundStyle(.secondary)
                Text("Changing the instrument calibrates it in the background the first time (a second for a flute, a minute or more for the brass and the saxophones); the old one plays on until then.")
                    .foregroundStyle(.secondary)
            }
            .padding(24)
            .frame(minWidth: 460, minHeight: 220, alignment: .topLeading)
        }
    }
}
