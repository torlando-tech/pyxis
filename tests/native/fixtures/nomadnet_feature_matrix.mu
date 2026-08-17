# NomadNet/Micron feature-conformance inventory (byte-exact UTF-8, LF only)
# canonical-commit: 89e3eea10c60d8fe597d36d2e091d5aab86bdfb8
# canonical-source-sha256: nomadnet/ui/textui/MicronParser.py c4b40918fe813a7cfbb696f33df8a08451fd0156a6919a185b75225f52402ffb
# canonical-source-sha256: nomadnet/ui/textui/Browser.py b7bc37e0fd4e72261703a037ab1967ea4cc43b837dc1cd74f92a835bacab40a1
# canonical-source-sha256: nomadnet/Node.py 2461a592b731cb1469bebb5ccc5f523892127881cb7a7e8ed586ac62a8c0c23a
# canonical-source-sha256: nomadnet/examples/various/input_fields.py c72e87170a3d833fc18b9c9d12299a4a056d8f46c4f8a43e3f872fc0b7b69dc9
# BEGIN AUTHORITATIVE
# Verbatim byte-exact excerpts from nomadnet/examples/various/input_fields.py.
# source-range: lines 18-18
# feature: custom-divider status: supported
-=
# source-range: lines 22-30
# feature: canonical-input-and-submit status: supported
An input field    : `B444`<username`Entered data>`b

An masked field   : `B444`<!|password`Value of Field>`b

An small field    : `B444`<8|small`test>`b, and some more text.

Two fields        : `B444`<8|one`One>`b `B444`<8|two`Two>`b

The data can be `!`[submitted`:/page/input_fields.mu`username|two]`!.
# source-range: lines 34-44
# feature: canonical-checkbox-radio status: supported
`B444`<?|sign_up|1|*`>`b Sign me up 

>> Radio group

Select your favorite color:

`B900`<^|color|Red`>`b  Red

`B090`<^|color|Green`>`b Green

`B009`<^|color|Blue`>`b Blue
# source-range: lines 49-57
# feature: canonical-submit-variants status: supported
You can `!`[submit`:/page/input_fields.mu`one|password|small|color]`! other fields, or just `!`[a single one`:/page/input_fields.mu`username]`!.

Or simply `!`[submit them all`:/page/input_fields.mu`*]`!.

Submission links can also `!`[include pre-configured variables`:/page/input_fields.mu`username|two|entitiy_id=4611|action=view]`!.

Or take all fields and `!`[pre-configured variables`:/page/input_fields.mu`*|entitiy_id=4611|action=view]`!.

Or only `!`[pre-configured variables`:/page/input_fields.mu`entitiy_id=4688|task=something]`!
# END AUTHORITATIVE
# BEGIN ADAPTED
# Focused vectors below are adapted from canonical syntax, not byte-exact excerpts.
# feature: default-divider status: supported
-
# feature: rgb-color status: supported
`F900RGB foreground`f and `B00ff00RGB background`b
# feature: grayscale-color status: supported
`Fg50Grayscale foreground`f and `Bg75Grayscale background`b
# feature: unknown-modifier status: supported
Unknown command is consumed: before`zafter
# feature: anchor status: supported
`:explicit-anchor
# feature: heading status: supported
>Primary heading
>>Secondary heading
# feature: table-delimiters status: supported
`tc80
Name|Value
:---|---:
alpha|one
`t
# feature: field-text status: supported
`<24|username`Entered data>
# feature: field-password status: supported
`<!|password`Secret value>
# feature: field-checkbox status: supported
`<?|sign_up|1|*`Sign me up>
# feature: field-radio status: supported
`<^|color|red|*`Red> `<^|color|blue`Blue>
# feature: submit-named status: supported
`[Submit named`:/page/form.mu`username|password|color]
# feature: submit-wildcard status: supported
`[Submit all`:/page/form.mu`*]
# feature: submit-preconfigured status: supported
`[Submit configured`:/page/form.mu`username|action=view|entity_id=4611]
# feature: partial-descriptor status: future
`{:/page/partial.mu`5`username|pid=matrix}
# END ADAPTED
# BEGIN SYNTHETIC
# Synthetic adversarial cases are intentionally outside the canonical excerpt.
# More than eight separators must retain only a bounded deterministic prefix.
`tl304
c0|c1|c2|c3|c4|c5|c6|c7|c8||||||||||||||||
---|---|---|---|---|---|---|---|---||||||||||||||||
v0|v1|v2|v3|v4|v5|v6|v7|v8||||||||||||||||
`t
# A partial placeholder remains visible/degraded but is not accepted as support.
`{:/page/missing.mu`0.1`*|pid=synthetic}
# END SYNTHETIC
