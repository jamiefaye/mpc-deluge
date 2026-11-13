# The upstream versions this project is built on. Sourced by tools/pristine.sh and
# tools/gen_headers.sh. Change them here and nowhere else, then run tools/vendor.sh; expect to
# repair patches/ by hand where upstream moved (see CLAUDE.md, "Which firmware version").
fw_version=1.3.0                                      # what the firmware's CMakeLists.txt declares at this commit
fw_commit=bef6d9df3f627f24fc3057a9efe15b567e234d83    # SynthstromAudible/DelugeFirmware, branch community (a nightly)
argon_commit=3779c5315a978a2f8c892e320ac12ecbb77c3759   # stellar-aria/argon, as the firmware's lib/CMakeLists.txt pins it
etl_tag=20.40.0                                       # ETLCPP/etl, likewise
