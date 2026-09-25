#include "particles_common.hlsli"

// ============================================
// КОНСТАНТЫ
// ============================================
cbuffer cbSim : register(b0)
{
    float3 gEmitterPos;
    float gDeltaTime;
    float3 gGravity;
    float gTotalTime;
    float gFloorY;
    float gBounce;
    uint gEmitCount;
    uint gMaxParticles;
    float gSpeedMin;
    float gSpeedMax;
    float gConeAngle;
    float gEmitterRadius;
    float gLifetimeMin;
    float gLifetimeMax;
    float gSizeMin;
    float gSizeMax;
};

// Сколько частиц было живо в начале кадра (копия счётчика входного буфера)
cbuffer cbAlive : register(b1)
{
    uint gAliveCount;
};

ConsumeStructuredBuffer<Particle> gInput : register(u0);
AppendStructuredBuffer<Particle> gOutput : register(u1);

static const float PI = 3.14159265f;

// ============================================
// ГЕНЕРАТОР СЛУЧАЙНЫХ ЧИСЕЛ (Wang hash)
// ============================================
uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16);
    seed *= 9u;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15);
    return seed;
}

float Rand(inout uint state)
{
    state = WangHash(state);
    return float(state) * (1.0f / 4294967296.0f);   // [0, 1)
}

// ============================================
// EMIT: рождение новых частиц фонтана
// ============================================
[numthreads(64, 1, 1)]
void Emit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gEmitCount)
        return;

    // Не переполняем буфер: Update добавит не больше gAliveCount частиц
    if (gAliveCount + id.x >= gMaxParticles)
        return;

    uint state = WangHash(id.x * 1973u + WangHash(asuint(gTotalTime)) * 9277u + 26699u);

    // Направление: конус вокруг оси Y
    float phi = Rand(state) * 2.0f * PI;
    float theta = sqrt(Rand(state)) * gConeAngle;   // sqrt — плотнее к краю, «зонтик» фонтана
    float3 dir = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));

    // Стартовая точка: небольшой диск вокруг сопла
    float r = sqrt(Rand(state)) * gEmitterRadius;
    float a = Rand(state) * 2.0f * PI;

    Particle p;
    p.Position = gEmitterPos + float3(r * cos(a), 0.0f, r * sin(a));
    p.Velocity = dir * lerp(gSpeedMin, gSpeedMax, Rand(state));
    p.Age = 0.0f;
    p.Lifetime = lerp(gLifetimeMin, gLifetimeMax, Rand(state));
    p.Size = lerp(gSizeMin, gSizeMax, Rand(state));

    // Цвет «воды»: от глубокого синего к почти белому
    p.Color = lerp(float3(0.10f, 0.35f, 0.90f), float3(0.75f, 0.90f, 1.00f), Rand(state));

    gOutput.Append(p);
}

// ============================================
// UPDATE: Consume из входного буфера -> физика -> Append выживших
// ============================================
[numthreads(256, 1, 1)]
void Update(uint3 id : SV_DispatchThreadID)
{
    // Consume из пустого буфера — неопределённое поведение, поэтому
    // ровно gAliveCount потоков забирают по одной частице
    if (id.x >= gAliveCount)
        return;

    Particle p = gInput.Consume();

    p.Age += gDeltaTime;
    if (p.Age >= p.Lifetime)
        return;                         // умерла — не добавляем в выходной буфер

    // Полу-неявный Эйлер
    p.Velocity += gGravity * gDeltaTime;
    p.Position += p.Velocity * gDeltaTime;

    // Отскок от пола
    float floorY = gFloorY + p.Size;
    if (p.Position.y < floorY && p.Velocity.y < 0.0f)
    {
        p.Position.y = floorY;
        p.Velocity.y = -p.Velocity.y * gBounce;
        p.Velocity.xz *= 0.7f;          // трение о пол
    }

    gOutput.Append(p);
}
