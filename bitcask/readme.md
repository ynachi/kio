```text
bitcask/
├── include/
│   └── bitcask/
│       ├── bitcask.h              # Main public API
│       ├── entry.h                # Entry format (CRC, timestamp, key, value)
│       ├── datafile.h             # Data file management
│       ├── hint_file.h            # Hint file for faster startup
│       ├── keydir.h               # In-memory hash index
│       ├── compaction.h           # Merge/compaction logic
│       ├── config.h               # Configuration options
│       └── errors.h               # Error types
├── src/
│   ├── bitcask.cpp
│   ├── entry.cpp
│   ├── datafile.cpp
│   ├── hint_file.cpp
│   ├── keydir.cpp
│   └── compaction.cpp
├── tests/
│   ├── bitcask_test.cpp
│   ├── entry_test.cpp
│   ├── compaction_test.cpp
│   └── benchmark.cpp
└── examples/
    └── basic_usage.cpp
```