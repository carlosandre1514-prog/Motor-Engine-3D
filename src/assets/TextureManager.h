#pragma once
#include <GLES3/gl32.h>
#include <tiny_gltf.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

namespace eng {

// Gerencia texturas na VRAM. Preferência: KTX (ASTC/ETC2 pré-comprimidos, 4-8x menos VRAM).
// Fallback: RGBA8 descomprimido (vindo de PNG/JPG do glTF) para desenvolvimento.
class TextureManager {
public:
    static TextureManager& instance() { static TextureManager t; return t; }

    GLuint white() { return white_ ? white_ : (white_ = solid(255, 255, 255, 255, false)); }
    GLuint flatNormal() { return flat_ ? flat_ : (flat_ = solid(128, 128, 255, 255, false)); }

    // Imagem decodificada pelo tinygltf (8 bits/canal)
    GLuint fromImage(const tinygltf::Image& img, bool srgb) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        GLenum fmt = img.component == 4 ? GL_RGBA : img.component == 3 ? GL_RGB : GL_RG;
        GLenum internal = img.component == 4 ? (srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8)
                        : img.component == 3 ? (srgb ? GL_SRGB8 : GL_RGB8) : GL_RG8;
        int levels = 1;
        for (int s = std::max(img.width, img.height); s > 1; s >>= 1) ++levels;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexStorage2D(GL_TEXTURE_2D, levels, internal, img.width, img.height);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width, img.height, fmt, GL_UNSIGNED_BYTE, img.image.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        setParams();
        return t;
    }

    // KTX1 com formato comprimido (ASTC 4x4..12x12 ou ETC2). Mipmaps já embutidos no arquivo.
    GLuint fromKtx(const uint8_t* data, size_t size) {
        struct Hdr { uint8_t id[12]; uint32_t endian, glType, glTypeSize, glFormat, glInternalFormat, glBaseInternalFormat,
                     w, h, depth, arrayElems, faces, mipLevels, kvBytes; };
        if (size < sizeof(Hdr)) return 0;
        Hdr h; std::memcpy(&h, data, sizeof(h));
        static const uint8_t magic[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
        if (std::memcmp(h.id, magic, 12) != 0 || h.glType != 0) return 0;  // glType 0 => comprimido
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        const uint8_t* p = data + sizeof(Hdr) + h.kvBytes;
        uint32_t w = h.w, hh = h.h;
        uint32_t levels = h.mipLevels ? h.mipLevels : 1;
        for (uint32_t l = 0; l < levels; ++l) {
            uint32_t imgSize; std::memcpy(&imgSize, p, 4); p += 4;
            glCompressedTexImage2D(GL_TEXTURE_2D, l, h.glInternalFormat, w, hh, 0, imgSize, p);
            p += (imgSize + 3) & ~3u;
            w = std::max(1u, w >> 1); hh = std::max(1u, hh >> 1);
        }
        setParams();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        return t;
    }

    // Escolha do formato no pipeline de build (basisu/toktx): ASTC se GL_KHR_texture_compression_astc_ldr,
    // senão ETC2 (garantido no GLES 3.0+).
    static bool supportsAstc() {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        return ext && std::strstr(ext, "GL_KHR_texture_compression_astc_ldr");
    }

private:
    GLuint solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb) {
        uint8_t px[4] = {r, g, b, a};
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        setParams();
        return t;
    }
    void setParams() {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    GLuint white_ = 0, flat_ = 0;
};

}  // namespace eng
