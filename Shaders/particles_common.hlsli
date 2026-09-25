// Общая структура частицы (48 байт) — совпадает с GpuParticle в ParticleSystem.h
struct Particle
{
    float3 Position;
    float Age;
    float3 Velocity;
    float Lifetime;
    float3 Color;
    float Size;
};
