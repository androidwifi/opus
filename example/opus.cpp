
#include <opus_types.h>
#include  <opus.h>
#include <cstring>
#include <memory>

#include <vector>
// https://github.com/mackron/dr_libs/blob/master/dr_wav.h
#define DR_WAV_IMPLEMENTATION

#include "dr_wav.h"

#define FRAME_SIZE 480
#define MAX_FRAME_SIZE (6*FRAME_SIZE)

#define MAX_CHANNELS 1
#define MAX_PACKET_SIZE (3*1276)

#pragma pack(push)
#pragma pack(1)

struct WavInfo {
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t bitsPerSample;
};

#pragma pack(pop)

#ifndef  nullptr
#define  nullptr NULL
#endif

class FileStream {
public:
    FileStream() {
        cur_pos = 0;
    }

    void Append(const char *data, size_t size) {
        if (cur_pos + size > Size()) {
            vec.resize(cur_pos + size);
        }
        memcpy(vec.data() + cur_pos, data, size);
        cur_pos += size;
    }

    void AppendU32(uint32_t val) {
        Append((char *) (&val), sizeof(val));
    }

    char *Data() {
        return vec.data();
    }

    size_t Size() {
        return vec.size();
    }

    size_t Read(void *buff, size_t elemSize, size_t elemCount) {
        size_t readed = std::min((vec.size() - cur_pos), (elemCount * elemSize)) / elemSize;
        if (readed > 0) {
            memcpy(buff, vec.data() + cur_pos, readed * elemSize);
            cur_pos += readed * elemSize;
        }
        return readed;
    }

    bool SeekCur(int offset) {
        if (cur_pos + offset > vec.size()) {
            cur_pos = !vec.empty() ? (vec.size() - 1) : 0;
            return false;
        } else {
            cur_pos += offset;
            return true;
        }
    }

    bool SeekBeg(int offset = 0) {
        cur_pos = 0;
        return SeekCur(offset);
    }

    bool WriteToFile(const char *filename) {
        FILE *fin = fopen(filename, "wb");
        if (!fin) {
            return false;
        }
        fseek(fin, 0, SEEK_SET);
        fwrite(vec.data(), sizeof(char), vec.size(), fin);
        fclose(fin);
        return true;
    }

    bool ReadFromFile(const char *filename) {
        FILE *fin = fopen(filename, "rb");
        if (!fin) {
            return false;
        }
        fseek(fin, 0, SEEK_END);
        long fileSize = ftell(fin);
        vec.resize(static_cast<unsigned long long int>(fileSize));
        fseek(fin, 0, SEEK_SET);
        fread(vec.data(), sizeof(char), vec.size(), fin);
        fclose(fin);
        return true;
    }

private:
    std::vector<char> vec;
    size_t cur_pos;
};

bool Wav2Opus(FileStream *input, FileStream *output);

bool Opus2Wav(FileStream *input, FileStream *output);

bool wav2stream(char *input, FileStream *output);

bool stream2wav(FileStream *input, char *output);


bool wavWrite_int16(char *filename, int16_t *buffer, int sampleRate, uint32_t totalSampleCount) {
    drwav_data_format format = {};
    format.container = drwav_container_riff;     // <-- drwav_container_riff = normal WAV files, drwav_container_w64 = Sony Wave64.
    format.format = DR_WAVE_FORMAT_PCM;          // <-- Any of the DR_WAVE_FORMAT_* codes.
    format.channels = 1;
    format.sampleRate = (drwav_uint32) sampleRate;
    format.bitsPerSample = 16;
    drwav *pWav = drwav_open_file_write(filename, &format);
    if (pWav) {
        drwav_uint64 samplesWritten = drwav_write(pWav, totalSampleCount, buffer);
        drwav_uninit(pWav);
        if (samplesWritten != totalSampleCount) {
            fprintf(stderr, "ERROR\n");
            return false;
        }
        return true;
    }
    return false;
}

int16_t *wavRead_int16(char *filename, uint32_t *sampleRate, uint64_t *totalSampleCount) {
    unsigned int channels;
    int16_t *buffer = drwav_open_and_read_file_s16(filename, &channels, sampleRate, totalSampleCount);
    if (buffer == nullptr) {
        fprintf(stderr, "ERROR\n");
        return nullptr;
    }
    if (channels != 1) {
        drwav_free(buffer);
        buffer = nullptr;
        *sampleRate = 0;
        *totalSampleCount = 0;
    }
    return buffer;
}

bool wav2stream(char *input, FileStream *output) {
    uint32_t sampleRate = 0;
    uint64_t totalSampleCount = 0;
    int16_t *wavBuffer = wavRead_int16(input, &sampleRate, &totalSampleCount);
    if (wavBuffer == nullptr) return false;
    WavInfo info = {};
    info.bitsPerSample = 16;
    info.sampleRate = sampleRate;
    info.channels = 1;
    output->SeekBeg();
    output->Append((char *) &info, sizeof(info));
    output->Append((char *) wavBuffer, totalSampleCount * sizeof(int16_t));
    free(wavBuffer);
    return true;
}

bool stream2wav(FileStream *input, char *output) {
    WavInfo info = {};
    input->SeekBeg();
    size_t read = input->Read(&info, sizeof(info), 1);
    if (read != 1) {
        return false;
    }
    size_t totalSampleCount = (input->Size() - sizeof(info)) / 2;
    return wavWrite_int16(output, (int16_t *) (input->Data() + sizeof(info)), info.sampleRate,
                          static_cast<uint32_t>(totalSampleCount));
}

bool Wav2Opus(FileStream *input, FileStream *output) {
    WavInfo in_info = {};
    input->SeekBeg();
    size_t read = input->Read(&in_info, sizeof(in_info), 1);
    if (read != 1) {
        return false;
    }
    uint32_t bitsPerSample = in_info.bitsPerSample;
    uint32_t sampleRate = in_info.sampleRate;
    uint16_t channels = in_info.channels;
    int err = 0;
    if (channels > MAX_CHANNELS) {
        return false;
    }
    OpusEncoder *encoder = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, &err);
    if (!encoder || err < 0) {
        fprintf(stderr, "failed to create an encoder: %s\n", opus_strerror(err));
        if (!encoder) {
            opus_encoder_destroy(encoder);
        }
        return false;
    }
    const uint16_t *data = (uint16_t *) (input->Data() + sizeof(in_info));
    size_t size = (input->Size() - sizeof(in_info)) / 2;
    opus_int16 pcm_bytes[FRAME_SIZE * MAX_CHANNELS];
    size_t index = 0;
    size_t step = static_cast<size_t>(FRAME_SIZE * channels);
    FileStream encodedData;
    unsigned char cbits[MAX_PACKET_SIZE];
    size_t frameCount = 0;
    size_t readCount = 0;
    while (index < size) {
        memset(&pcm_bytes, 0, sizeof(pcm_bytes));
        if (index + step <= size) {
            memcpy(pcm_bytes, data + index, step * sizeof(uint16_t));
            index += step;
        } else {
            readCount = size - index;
            memcpy(pcm_bytes, data + index, (readCount) * sizeof(uint16_t));
            index += readCount;
        }
        int nbBytes = opus_encode(encoder, pcm_bytes, channels * FRAME_SIZE, cbits, MAX_PACKET_SIZE);
        if (nbBytes < 0) {
            fprintf(stderr, "encode failed: %s\n", opus_strerror(nbBytes));
            break;
        }
        ++frameCount;
        encodedData.AppendU32(static_cast<uint32_t>(nbBytes));
        encodedData.Append((char *) cbits, static_cast<size_t>(nbBytes));
    }
    WavInfo info = {};
    info.bitsPerSample = bitsPerSample;
    info.sampleRate = sampleRate;
    info.channels = channels;
    output->SeekBeg();
    output->Append((char *) &info, sizeof(info));
    output->Append(encodedData.Data(), encodedData.Size());
    opus_encoder_destroy(encoder);
    return true;
}

bool Opus2Wav(FileStream *input, FileStream *output) {
    WavInfo info = {};
    input->SeekBeg();
    size_t read = input->Read(&info, sizeof(info), 1);
    if (read != 1) {
        return false;
    }
    int channels = info.channels;
    if (channels > MAX_CHANNELS) {
        return false;
    }
    output->SeekBeg();
    output->Append((char *) &info, sizeof(info));
    int err = 0;
    OpusDecoder *decoder = opus_decoder_create(info.sampleRate, channels, &err);
    if (!decoder || err < 0) {
        fprintf(stderr, "failed to create decoder: %s\n", opus_strerror(err));
        if (!decoder) {
            opus_decoder_destroy(decoder);
        }
        return false;
    }
    unsigned char cbits[MAX_PACKET_SIZE];
    opus_int16 out[MAX_FRAME_SIZE * MAX_CHANNELS];
    int frameCount = 0;
    while (true) {
        uint32_t nbBytes;
        size_t readed = input->Read(&nbBytes, sizeof(uint32_t), 1);
        if (readed == 0) {
            break;
        }

        if (nbBytes > sizeof(cbits)) {
            fprintf(stderr, "nbBytes > sizeof(cbits)\n");
            break;
        }
        readed = input->Read(cbits, sizeof(char), nbBytes);
        if (readed != nbBytes) {
            fprintf(stderr, "readed != nbBytes\n");
            break;
        }
        int frame_size = opus_decode(decoder, cbits, nbBytes, out, MAX_FRAME_SIZE, 0);
        if (frame_size < 0) {
            fprintf(stderr, "decoder failed: %s\n", opus_strerror(frame_size));
            break;
        }
        ++frameCount;
        output->Append((char *) out, channels * frame_size * sizeof(out[0]));
    }
    opus_decoder_destroy(decoder);
    return true;
}


void splitpath(const char *path, char *drv, char *dir, char *name, char *ext) {
    const char *end;
    const char *p;
    const char *s;
    if (path[0] && path[1] == ':') {
        if (drv) {
            *drv++ = *path++;
            *drv++ = *path++;
            *drv = '\0';
        }
    } else if (drv)
        *drv = '\0';
    for (end = path; *end && *end != ':';)
        end++;
    for (p = end; p > path && *--p != '\\' && *p != '/';)
        if (*p == '.') {
            end = p;
            break;
        }
    if (ext)
        for (s = end; (*ext = *s++);)
            ext++;
    for (p = end; p > path;)
        if (*--p == '\\' || *p == '/') {
            p++;
            break;
        }
    if (name) {
        for (s = p; s < end;)
            *name++ = *s++;
        *name = '\0';
    }
    if (dir) {
        for (s = path; s < p;)
            *dir++ = *s++;
        *dir = '\0';
    }
}

void opus2wav(const char *in_file, char *out_file) {
    FileStream input;
    FileStream output;
    input.ReadFromFile(in_file);
    Opus2Wav(&input, &output);
    stream2wav(&output, out_file);
}

void wav2opus(char *in_file, char *out_file) {
    FileStream input;
    FileStream output;
    wav2stream(in_file, &input);
    Wav2Opus(&input, &output);
    output.WriteToFile(out_file);
}

int main(int argc, char *argv[]) {
    printf("Audio Noise Reduction\n");
    printf("blog:http://tntmonks.cnblogs.com/\n");
    printf("e-mail:gaozhihan@vip.qq.com\n");
    if (argc < 2)
        return -1;
    char *in_file = argv[1];
    char drive[3];
    char dir[256];
    char fname[256];
    char ext[256];
    char out_file[1024];
    splitpath(in_file, drive, dir, fname, ext);
    if (memcmp(".wav", ext, strlen(ext)) == 0) {
        sprintf(out_file, "%s%s%s.out", drive, dir, fname);
        wav2opus(in_file, out_file);
    } else if (memcmp(".out", ext, strlen(ext)) == 0) {
        sprintf(out_file, "%s%s%s_out.wav", drive, dir, fname);
        opus2wav(in_file, out_file);
    }
    printf("done.\n");
    printf("press any key to exit.\n");
    getchar();
    return 0;
}
