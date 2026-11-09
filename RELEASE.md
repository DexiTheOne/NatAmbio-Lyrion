NatAmbio Alpha 0.1.7 fixes intermittent delays when switching tracks on Lyrion. A new stream can start while the previous DSP helper closes. When the previous track has already saved its DSP history, the next track can still use it. If the streams overlap before that history is ready, the new track starts with fresh DSP state.

If NatAmbio Alpha is already installed, use Lyrion's Manage Plugins page to apply the update.
