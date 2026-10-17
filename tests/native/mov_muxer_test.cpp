// Muxes recorded samples with MovMuxer, as NativeAvRecorder does.
// usage: mov_muxer_helper out hevc_csd.bin aac_asc.bin samples.bin width height fps rate channels rotation pcm_bits
// pcm_bits 0 = AAC in MP4; 16/24 = PCM in QuickTime MOV (audio records then hold raw PCM).
// Optional: index_path crash_after — keep a sample index; when crash_after > 0, stop after that
// many samples without finish() (as if the app died) and then run MovMuxer::recover().
// samples.bin: repeated [u8 track (0 video, 1 audio)][u8 sync][i64 ptsUs][u32 size][bytes], little-endian.
// tests/mov_muxer.py builds the inputs from ffmpeg output and checks the result with ffprobe.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "video/MovMuxer.h"

using rawrcam::video::MovMuxer;

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc != 12 && argc != 14) {
        std::fprintf(stderr, "usage: %s out csd asc samples width height fps rate channels rotation pcm_bits\n", argv[0]);
        return 2;
    }
    const int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 2;
    try {
        const int pcmBits = std::stoi(argv[11]);
        const char* indexPath = argc == 14 ? argv[12] : nullptr;
        const long crashAfter = argc == 14 ? std::stol(argv[13]) : 0;
        const int indexFd = indexPath ? open(indexPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644) : -1;
        auto owned = std::make_unique<MovMuxer>(fd, pcmBits > 0 ? MovMuxer::Brand::QuickTime : MovMuxer::Brand::Mp4,
                                                indexFd);
        MovMuxer& muxer = *owned;
        MovMuxer::Video video;
        video.annexBConfig = slurp(argv[2]);
        video.width = std::stoi(argv[5]);
        video.height = std::stoi(argv[6]);
        video.fps = std::stoi(argv[7]);
        video.rotationDegrees = std::stoi(argv[10]);
        video.colorStandard = 1;
        video.colorTransfer = 3;
        video.colorRange = 2;
        MovMuxer::Audio audio;
        audio.aacConfig = slurp(argv[3]);
        audio.sampleRate = std::stoi(argv[8]);
        audio.channels = std::stoi(argv[9]);
        audio.bitrate = 128'000;
        if (pcmBits > 0) {
            audio.codec = MovMuxer::Audio::Codec::Pcm;
            audio.bitsPerSample = pcmBits;
        }
        // Like the recorder: whichever encoder reports its format first.
        muxer.setAudio(audio);
        muxer.setVideo(video);
        const auto samples = slurp(argv[4]);
        size_t pos = 0;
        long written = 0;
        while (pos + 14 <= samples.size()) {
            if (crashAfter > 0 && written++ == crashAfter) {
                owned.reset();  // No finish(): unflushed staging data is lost, as in a crash.
                const int indexRead = open(indexPath, O_RDONLY);
                MovMuxer::recover(fd, indexRead);
                close(indexRead);
                close(fd);
                return 0;
            }
            const uint8_t track = samples[pos], sync = samples[pos + 1];
            int64_t pts = 0;
            uint32_t size = 0;
            std::memcpy(&pts, samples.data() + pos + 2, 8);
            std::memcpy(&size, samples.data() + pos + 10, 4);
            pos += 14;
            if (track == 0) muxer.writeVideo(samples.data() + pos, size, pts, sync != 0);
            else muxer.writeAudio(samples.data() + pos, size, pts);
            pos += size;
        }
        muxer.finish();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
