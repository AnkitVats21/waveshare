enum class ChunkType : uint8_t {
    DATA,
    EOF_STREAM,
    ERROR
};

struct AudioChunkHeader {
    ChunkType type;
    uint32_t size;
};

static constexpr size_t AUDIO_CHUNK_SIZE = 32 * 1024;
