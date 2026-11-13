# The upstream versions this project is built on. Sourced by tools/pristine.sh and
# tools/gen_headers.sh. Change them here and nowhere else, then run tools/vendor.sh; expect to
# repair patches/ by hand where upstream moved (see CLAUDE.md, "Which firmware version").
fw_version=1.3.0                                      # SynthstromAudible/DelugeFirmware, tag v1.3.0 (branch release/1.3)
fw_commit=156500e7c4b756840bd4b116e70a3b27c27d64bb
argon_commit=4b8efc4f4f21595d454e415fb619dd91223c0400   # stellar-aria/argon, as the firmware's lib/CMakeLists.txt pins it
etl_tag=20.40.0                                       # ETLCPP/etl, likewise
