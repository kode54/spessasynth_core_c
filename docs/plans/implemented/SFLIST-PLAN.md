- Draft up a modification plan for implementing sflist support in the player.

- A partial attempt is implemented in the json reader and builder in spessasynth_core/extern/json and
  spessasynth_core/src/sflist.

- Preset rules should support the following:
    - Source program number, or -1 for all programs
    - Source bank number, which is (MSB) in the low 8 bits and (LSB) in the upper 8 bits, or
      -1 for all banks in the source file
    - Destination program number. If source program number is positive, it's a bank->output
      remapping. If source program number is -1, all program numbers will be incremented by
      the destination program number.
    - Destination bank number, same MSB/LSB rules as source bank number. If source bank number
      is positive, only the source bank will be processed from input presets, mapping it directly
      to the destination bank number in the output. If source bank number is -1, then all banks
      in the source bank will be offset forward by the destination bank number.
    - Channel number mapping. Currently I try to use minimum channel and channel count, but
      maybe there is a more optimal way of conveying a mixed number of channels, even if it
      may be rare to do more than one channel, or other than all channels.

- There should be a function for taking one SS_SoundBank and one or more accompanying filter
  rules, and producing a SS_BasicPreset list, which is a direct copy of the source bank's
  preset list, except that the program and/or bank_msb and/or bank_lsb fields have been
  altered according to the rules. Or, the presets may be whittled down to only a handful of
  presets that match the source program and/or bank rules specified. Upon closing, the only
  thing that should be freed is the SS_BasicPreset list, as the original pointers are merely
  duplicated from the source bank. The bank itself should also be freed.

- The functions which accept bare SS_SoundBank loading in the processor.c should be adapted
  to produce a filtered bank list preset, except using a blank placeholder filter preset of
  source program -1, source bank -1, destination preset 0, destination bank filled with the
  current bank_offset parameter. The insert parameter should push the specified bank to the
  top of the loaded filtered banks list.

- The ID parameter for freeing banks or preset lists, should be tied to the entire preset
  list being inserted or appended. Replacement should be handled by calling the remove
  function with the given ID to remove all presets using that ID first, in case the ID
  refers to an entire range of presets.

- The soundbank.c function for retrieving a preset from the loaded set should take a list
  of filtered preset containers, and parse them similar to how it currently parses the
  SS_SoundBank lists. The preset containers will contain the `SS_SoundBank *parent_bank`,
  which won't be used by the preset matching, as well as a `SS_BasicPreset *presets` list,
  and a `size_t preset_count` that indicates how many presets have been filtered into the
  list. It will parse through all of the incoming preset sets in load order, similar to
  how it already parses through the SS_SoundBank lists. Due to how the filtered presets
  will already have their bank_offset applied to the `SS_BasicPreset`s parameters on load,
  there will not need to be any offsetting at search time.

- The remove function for unloading banks or preset lists by ID field, its public facing
  version will have a parameter to specify whether it should free the filtered presets
  upon removal. If freeing it, it should free it similar to how the sflist draft frees it,
  by freeing the SS_BasicPreset pointer, as nothing within it needs to be freed in a
  nested fashion. Then the SS_SoundBank should be freed, then the preset container itself
  should be freed. Otherwise, with the dontfree parameter set, all of these presets should
  merely be removed from the loaded list and the list shrunken down by the removed count,
  but not its container memory reduced. Only the number of loaded filtered preset containers
  reduced.
