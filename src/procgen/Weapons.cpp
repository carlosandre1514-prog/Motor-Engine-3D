#include <glm/gtc/matrix_transform.hpp>
#include "procgen/Buildings.h"
#include "procgen/Character.h"

namespace eng {

// Todas as medidas em metros. Cano para +Z, topo +Y, mão na origem.
glm::vec3 buildWeapon(Geo& g, int kind, bool stock) {
    MeshBuilder& steel = g[Mat::GunMetal];
    MeshBuilder& bright = g[Mat::Metal];
    MeshBuilder& poly = g[Mat::Polymer];
    MeshBuilder& wood = g[Mat::Wood];
    const glm::mat4 base = steel.xf;
    auto tilt = [&](MeshBuilder& b, glm::vec3 pivot, float ang) {
        b.xf = base * glm::translate(glm::mat4(1.f), pivot) * glm::rotate(glm::mat4(1.f), ang, glm::vec3(1, 0, 0)) *
               glm::translate(glm::mat4(1.f), -pivot);
    };
    auto reset = [&](MeshBuilder& b) { b.xf = base; };
    switch (kind) {
        case WK_Pistol: {
            steel.boxC({0, 0.06f, 0.07f}, {0.032f, 0.034f, 0.19f});        // ferrolho
            poly.boxC({0, 0.033f, 0.065f}, {0.03f, 0.022f, 0.17f});         // armação
            tilt(poly, {0, 0.02f, 0}, -0.22f);
            poly.boxC({0, -0.035f, -0.01f}, {0.03f, 0.11f, 0.046f});        // cabo
            reset(poly);
            poly.boxC({0, 0.008f, 0.04f}, {0.008f, 0.03f, 0.04f});          // guarda-mato
            steel.boxC({0, 0.08f, 0.15f}, {0.006f, 0.008f, 0.008f});        // massa de mira
            return {0, 0.06f, 0.166f};
        }
        case WK_Rifle: {
            steel.boxC({0, 0.045f, 0.1f}, {0.05f, 0.07f, 0.34f});           // caixa da culatra
            bright.cylinder({0, 0.06f, 0.27f}, {0, 0.06f, 0.72f}, 0.012f, 0.012f, 8);   // cano
            steel.cylinder({0, 0.085f, 0.3f}, {0, 0.085f, 0.52f}, 0.01f, 0.01f, 6);     // tubo de gás
            wood.boxC({0, 0.05f, 0.37f}, {0.056f, 0.05f, 0.18f});           // guarda-mão
            steel.boxC({0, 0.1f, 0.66f}, {0.008f, 0.04f, 0.01f});           // mira
            tilt(wood, {0, 0.03f, -0.07f}, 0.12f);
            if (stock) wood.boxC({0, 0.025f, -0.23f}, {0.042f, 0.085f, 0.32f});  // coronha
            reset(wood);
            tilt(poly, {0, 0.f, 0}, -0.25f);
            poly.boxC({0, -0.04f, 0.f}, {0.03f, 0.1f, 0.04f});              // empunhadura
            reset(poly);
            tilt(steel, {0, 0.02f, 0.17f}, 0.35f);
            steel.boxC({0, -0.07f, 0.18f}, {0.03f, 0.17f, 0.065f});         // carregador curvo
            reset(steel);
            return {0, 0.06f, 0.73f};
        }
        case WK_Shotgun: {
            steel.boxC({0, 0.045f, 0.09f}, {0.05f, 0.07f, 0.22f});
            bright.cylinder({0, 0.065f, 0.19f}, {0, 0.065f, 0.78f}, 0.016f, 0.016f, 10);
            steel.cylinder({0, 0.035f, 0.19f}, {0, 0.035f, 0.66f}, 0.013f, 0.013f, 8);   // tubo do carregador
            wood.boxC({0, 0.033f, 0.43f}, {0.05f, 0.048f, 0.17f});          // telha (pump)
            tilt(wood, {0, 0.03f, -0.02f}, 0.15f);
            if (stock) wood.boxC({0, 0.01f, -0.19f}, {0.045f, 0.1f, 0.34f});  // coronha
            reset(wood);
            return {0, 0.065f, 0.79f};
        }
        case WK_Sniper: {
            steel.boxC({0, 0.045f, 0.12f}, {0.05f, 0.06f, 0.3f});
            bright.cylinder({0, 0.055f, 0.27f}, {0, 0.055f, 0.98f}, 0.014f, 0.011f, 8);
            poly.cylinder({0, 0.12f, -0.02f}, {0, 0.12f, 0.32f}, 0.022f, 0.022f, 12);    // luneta
            poly.cylinder({0, 0.12f, 0.26f}, {0, 0.12f, 0.34f}, 0.022f, 0.03f, 12);
            steel.boxC({0, 0.09f, 0.05f}, {0.012f, 0.04f, 0.015f});
            steel.boxC({0, 0.09f, 0.22f}, {0.012f, 0.04f, 0.015f});
            tilt(poly, {0, 0.f, 0}, -0.2f);
            poly.boxC({0, -0.04f, 0.f}, {0.03f, 0.1f, 0.04f});
            reset(poly);
            if (stock) poly.boxC({0, 0.02f, -0.25f}, {0.045f, 0.11f, 0.36f});  // coronha
            steel.boxC({0, -0.03f, 0.13f}, {0.03f, 0.06f, 0.07f});          // carregador
            return {0, 0.055f, 0.99f};
        }
        case WK_Knife: {
            poly.boxC({0, 0.f, 0.f}, {0.026f, 0.032f, 0.11f});
            steel.boxC({0, 0.f, 0.06f}, {0.05f, 0.014f, 0.01f});
            bright.boxC({0, 0.004f, 0.15f}, {0.004f, 0.03f, 0.17f});
            return {0, 0, 0.24f};
        }
        default: {  // taco de beisebol
            wood.cylinder({0, 0, -0.1f}, {0, 0, 0.12f}, 0.016f, 0.018f, 10);
            wood.cylinder({0, 0, 0.12f}, {0, 0, 0.72f}, 0.018f, 0.036f, 10);
            poly.cylinder({0, 0, -0.1f}, {0, 0, 0.05f}, 0.019f, 0.019f, 10, false);   // fita
            return {0, 0, 0.72f};
        }
    }
}

}  // namespace eng
