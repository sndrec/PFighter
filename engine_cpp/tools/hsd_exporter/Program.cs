using System.Text;
using HSDRaw;
using HSDRaw.Common;
using HSDRaw.Common.Animation;
using HSDRaw.GX;
using HSDRaw.Melee;
using HSDRaw.Melee.Cmd;
using HSDRaw.Melee.Gr;
using HSDRaw.Melee.Pl;
using HSDRaw.Tools;
using HSDRaw.Tools.Melee;

static byte TrackId(JointTrackType track) => track switch
{
    JointTrackType.HSD_A_J_TRAX => 0,
    JointTrackType.HSD_A_J_TRAY => 1,
    JointTrackType.HSD_A_J_TRAZ => 2,
    JointTrackType.HSD_A_J_ROTX => 3,
    JointTrackType.HSD_A_J_ROTY => 4,
    JointTrackType.HSD_A_J_ROTZ => 5,
    JointTrackType.HSD_A_J_SCAX => 6,
    JointTrackType.HSD_A_J_SCAY => 7,
    JointTrackType.HSD_A_J_SCAZ => 8,
    _ => 255,
};

static byte InterpolationId(object interpolation) => interpolation.ToString() switch
{
    "HSD_A_OP_CON" => 0,
    "HSD_A_OP_LIN" => 1,
    "HSD_A_OP_SPL" => 2,
    "HSD_A_OP_SPL0" => 2,
    "HSD_A_OP_SLP" => 2,
    _ => 1,
};

static bool IsRotationTrack(JointTrackType track) =>
    track is JointTrackType.HSD_A_J_ROTX or JointTrackType.HSD_A_J_ROTY or JointTrackType.HSD_A_J_ROTZ;

static void WriteString(BinaryWriter writer, string value)
{
    byte[] bytes = Encoding.UTF8.GetBytes(value);
    writer.Write(bytes.Length);
    writer.Write(bytes);
}

static void WriteVec3(BinaryWriter writer, float x, float y, float z)
{
    writer.Write(x);
    writer.Write(y);
    writer.Write(z);
}

static void WriteFighterAttributesBinary(BinaryWriter writer, SBM_CommonFighterAttributes? attr)
{
    writer.Write(attr is not null);
    if (attr is null)
    {
        return;
    }
    writer.Write(attr.InitialWalkSpeed);
    writer.Write(attr.WalkAcceleration);
    writer.Write(attr.MaxWalkSpeed);
    writer.Write(attr.MidWalkPoint);
    writer.Write(attr.FastWalkSpeed);
    writer.Write(attr.Friction);
    writer.Write(attr.InitialDashSpeed);
    writer.Write(attr.StopTurnInitialSpeedA);
    writer.Write(attr.StopTurnInitialSpeedB);
    writer.Write(attr.InitialRunSpeed);
    writer.Write(attr.RunAnimationScale);
    writer.Write(attr.DashLockoutDirection);
    writer.Write(attr.DashDurationBeforeRunning);
    writer.Write(attr.JumpStartupLag);
    writer.Write(attr.InitialHorizontalJumpVelocity);
    writer.Write(attr.InitialVerticalJumpVelocity);
    writer.Write(attr.GroundToAirJumpMomentumMultiplier);
    writer.Write(attr.MaximumShorthopHorizontalVelocity);
    writer.Write(attr.MaximumShorthopVerticalVelocity);
    writer.Write(attr.VerticalAirJumpMultiplier);
    writer.Write(attr.HorizontalAirJumpMultiplier);
    writer.Write(attr.NumberOfJumps);
    writer.Write(attr.Gravity);
    writer.Write(attr.TerminalVelocity);
    writer.Write(attr.AerialSpeed);
    writer.Write(attr.AerialFriction);
    writer.Write(attr.MaxAerialHorizontalSpeed);
    writer.Write(attr.AirFriction);
    writer.Write(attr.FastFallTerminalVelocity);
    writer.Write(attr.TiltTurnForcedVelocity);
    writer.Write(attr.FramesToChangeDirectionOnStandingTurn);
    writer.Write(attr.Weight);
    writer.Write(attr.ModelScale);
    writer.Write(attr.ShieldSize);
    writer.Write(attr.ShieldBreakInitialVelocity);
    writer.Write(attr.NormalLandingLag);
    writer.Write(attr.NairLandingLag);
    writer.Write(attr.FairLandingLag);
    writer.Write(attr.BairLandingLag);
    writer.Write(attr.UairLandingLag);
    writer.Write(attr.DairLandingLag);
    writer.Write(attr.WallJumpHorizontalVelocity);
    writer.Write(attr.WallJumpVerticalVelocity);
    writer.Write(attr.LedgeJumpHorizontalVelocity);
    writer.Write(attr.LedgeJumpVerticalVelocity);
}

const int CommonBoneLookupCount = 0x36;

static int[] EmptyCommonBoneLookup()
{
    int[] lookup = new int[CommonBoneLookupCount];
    Array.Fill(lookup, -1);
    return lookup;
}

static SBM_ftLoadCommonData? TryLoadCommonDataForFighter(string fighterDatPath)
{
    string? directory = Path.GetDirectoryName(fighterDatPath);
    string commonPath = Path.Combine(directory ?? "", "PlCo.dat");
    if (!File.Exists(commonPath))
    {
        return null;
    }

    HSDRawFile commonFile = new(commonPath);
    return commonFile.Roots.Select(root => root.Data).OfType<SBM_ftLoadCommonData>().FirstOrDefault();
}

static int[] ReadCommonBoneLookup(SBM_ftLoadCommonData? commonData, int boneTableIndex)
{
    int[] lookup = EmptyCommonBoneLookup();
    HSDFixedLengthPointerArrayAccessor<SBM_BoneLookupTable>? boneTables = commonData?.BoneTables;
    if (boneTables is null || boneTableIndex < 0 || boneTableIndex >= boneTables.Length)
    {
        return lookup;
    }

    SBM_BoneLookupTable? table = boneTables[boneTableIndex];
    HSDAccessor? commonAttribute = table?._s.GetReference<HSDAccessor>(0x04);
    if (commonAttribute is null)
    {
        return lookup;
    }

    for (int i = 0; i < CommonBoneLookupCount && i < commonAttribute._s.Length; ++i)
    {
        byte bone = commonAttribute._s.GetByte(i);
        lookup[i] = bone == 0xFF ? -1 : bone;
    }
    return lookup;
}

static int[] ReverseCommonBoneLookup(int[] partToJoint)
{
    int maxJoint = -1;
    foreach (int joint in partToJoint)
    {
        if (joint >= 0)
        {
            maxJoint = Math.Max(maxJoint, joint);
        }
    }

    if (maxJoint < 0)
    {
        return Array.Empty<int>();
    }

    int[] jointToPart = new int[maxJoint + 1];
    Array.Fill(jointToPart, -1);
    for (int part = 0; part < partToJoint.Length; ++part)
    {
        int joint = partToJoint[part];
        if (joint >= 0 && joint < jointToPart.Length)
        {
            jointToPart[joint] = part;
        }
    }
    return jointToPart;
}

static int FighterKindForDatPath(string fighterDatPath)
{
    string name = Path.GetFileNameWithoutExtension(fighterDatPath);
    if (name.StartsWith("Pl", StringComparison.OrdinalIgnoreCase))
    {
        name = name[2..];
    }
    if (name.Length > 2)
    {
        name = name[..2];
    }

    return name.ToLowerInvariant() switch
    {
        "mr" => 0,
        "fx" => 1,
        "ca" => 2,
        "dk" => 3,
        "kb" => 4,
        "kp" => 5,
        "lk" => 6,
        "sk" => 7,
        "ns" => 8,
        "pe" => 9,
        "pp" => 10,
        "nn" => 11,
        "pk" => 12,
        "ss" => 13,
        "ys" => 14,
        "pr" => 15,
        "mt" => 16,
        "lg" => 17,
        "ms" => 18,
        "zd" => 19,
        "cl" => 20,
        "dr" => 21,
        "fc" => 22,
        "pc" => 23,
        "gw" => 24,
        "gn" => 25,
        "fe" => 26,
        _ => -1,
    };
}

static int RemapFigaNodeToTargetJoint(SBM_ftLoadCommonData? commonData, uint actionFlags, int targetFighterKind, int sourceNode)
{
    int sourceBoneTableIndex = (int)(actionFlags & 0x3F);
    if (commonData is null || targetFighterKind < 0 || sourceBoneTableIndex == targetFighterKind)
    {
        return sourceNode;
    }

    int[] sourceLookup = ReadCommonBoneLookup(commonData, sourceBoneTableIndex);
    int[] targetLookup = ReadCommonBoneLookup(commonData, targetFighterKind);
    int[] sourceReverse = ReverseCommonBoneLookup(sourceLookup);
    if (sourceNode < 0 || sourceNode >= sourceReverse.Length)
    {
        return -1;
    }

    int part = sourceReverse[sourceNode];
    if (part < 0 || part >= targetLookup.Length)
    {
        return -1;
    }
    return targetLookup[part];
}

static void WriteCommonDataBinary(string outputPath, string commonDatPath)
{
    HSDRawFile commonFile = new(commonDatPath);
    SBM_ftLoadCommonData commonData = commonFile.Roots.Select(root => root.Data).OfType<SBM_ftLoadCommonData>().First();
    ftLoadCommandDataCommonAttributes attr = commonData.CommonAttributes
        ?? throw new InvalidDataException("PlCo.dat is missing ftLoadCommonData common attributes");

    float F(int offset) => attr._s.GetFloat(offset);
    int I(int offset) => attr._s.GetInt32(offset);
    int R(int offset) => (int)MathF.Round(F(offset));

    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputPath)) ?? ".");
    using FileStream stream = File.Create(outputPath);
    using BinaryWriter writer = new(stream, Encoding.UTF8);
    writer.Write(Encoding.ASCII.GetBytes("PFCM"));

    writer.Write(F(0x08));
    writer.Write(F(0x0C));
    writer.Write(F(0x24));
    writer.Write(F(0x28));
    writer.Write(F(0x2C));
    writer.Write(F(0x30));
    writer.Write(F(0x34));
    writer.Write(F(0x38));
    writer.Write(F(0x3C));
    writer.Write(I(0x40));
    writer.Write(R(0x44));
    writer.Write(R(0x48));
    writer.Write(R(0x4C));
    writer.Write(F(0x54));
    writer.Write(F(0x58));
    writer.Write(F(0x5C));
    writer.Write(F(0x60));
    writer.Write(F(0x6C));
    writer.Write(F(0x70));
    writer.Write(I(0x74));
    writer.Write(F(0x78));
    writer.Write(F(0x7C));
    writer.Write(F(0x80));
    writer.Write(F(0x88));
    writer.Write(I(0x8C));
    writer.Write(F(0x90));
    writer.Write(F(0x94));
    writer.Write(F(0x260));
    writer.Write(F(0x264));
    writer.Write(R(0x268));
    writer.Write(F(0x278));
    writer.Write(F(0x27C));
    writer.Write(F(0x284));
    writer.Write(F(0x288));
    writer.Write(F(0x28C));
    writer.Write(F(0x290));
    writer.Write(F(0x294));
    writer.Write(F(0x298));
    writer.Write(I(0x2A0));
    writer.Write(I(0x2B8));
    writer.Write(F(0x2D4));
    writer.Write(F(0x2D8));
    writer.Write(F(0x2DC));
    writer.Write(F(0x2E0));
    writer.Write(F(0x2E4));
    writer.Write(F(0x2E8));
    writer.Write(F(0x2EC));
    writer.Write(F(0x2F0));
    writer.Write(F(0x2F4));
    writer.Write(F(0x3E0));
    writer.Write(F(0x3E4));
    writer.Write(F(0x3E8));
    writer.Write(F(0x3EC));
    writer.Write(F(0x354));
    writer.Write(F(0x358));
    writer.Write(F(0x35C));
    writer.Write(F(0x360));
    writer.Write(F(0x364));
    writer.Write(F(0x368));
    writer.Write(F(0x36C));
    writer.Write(F(0x370));
    writer.Write(F(0x374));
    writer.Write(F(0x378));
    writer.Write(F(0x37C));
    writer.Write(F(0x3A4));
    writer.Write(F(0x3A8));
    writer.Write(F(0x3AC));
    writer.Write(F(0x3B0));
    writer.Write(F(0x3B4));
    writer.Write(F(0x3B8));
    writer.Write(F(0x3BC));
    writer.Write(F(0x3C4));
    writer.Write(F(0x3C8));
    writer.Write(F(0x314));
    writer.Write(I(0x318));
    writer.Write(F(0x31C));
    writer.Write(I(0x320));
    writer.Write(I(0x324));
    writer.Write(F(0x32C));
    writer.Write(F(0x330));
    writer.Write(I(0x334));
    writer.Write(F(0x338));
    writer.Write(F(0x33C));
    writer.Write(F(0x340));
    writer.Write(R(0x344));
    writer.Write(I(0x410));
    writer.Write(R(0x424));
    writer.Write(F(0x42C));
    writer.Write(R(0x430));
    writer.Write(F(0x438));
    writer.Write(F(0x440));
    writer.Write(F(0x4B0));
    writer.Write(I(0x4B4));
    writer.Write(F(0x4B8));
    writer.Write(F(0x4BC));
    writer.Write(F(0x4C0));
    writer.Write(F(0x464));
    writer.Write(R(0x468));
    writer.Write(F(0x46C));
    writer.Write(R(0x470));
    writer.Write(F(0x474));
    writer.Write(F(0x478));
    writer.Write(F(0x47C));
    writer.Write(F(0x480));
    writer.Write(F(0x494));
    writer.Write(I(0x498));
    writer.Write(I(0x760));
    writer.Write(I(0x764));
    writer.Write(R(0x768));
    writer.Write(F(0x76C));
    writer.Write(R(0x770));
    writer.Write(I(0x774));
    writer.Write(F(0x778));
    writer.Write(MathF.Tan(F(0x20)));
    writer.Write(F(0xDC));
    writer.Write(F(0xE0));
    writer.Write(F(0x44C));
    writer.Write(F(0x98));
    writer.Write(F(0x9C));
    writer.Write(F(0xA0));
    writer.Write(F(0xA4));
    writer.Write(F(0xA8));
    writer.Write(F(0xAC));
    writer.Write(F(0xB0));
    writer.Write(F(0xB8));
    writer.Write(F(0xBC));
    writer.Write(F(0xC0));
    writer.Write(F(0xC4));
    writer.Write(F(0xCC));
    writer.Write(R(0xD0));
    writer.Write(F(0xD4));
    writer.Write(R(0xD8));
    writer.Write(I(0xE4));
    writer.Write(F(0xE8));
    writer.Write(F(0xF4));
    writer.Write(F(0xF8));
    writer.Write(F(0x100));
    writer.Write(F(0x108));
    writer.Write(F(0x10C));
    writer.Write(F(0x110));
    writer.Write(F(0x114));
    writer.Write(F(0x118));
    writer.Write(F(0x11C));
    writer.Write(F(0x120));
    writer.Write(F(0x144));
    writer.Write(F(0x148));
    writer.Write(F(0x14C));
    writer.Write(F(0x150));
    writer.Write(F(0x154));
    writer.Write(F(0x158));
    writer.Write(F(0x15C));
    writer.Write(F(0x160));
    writer.Write(F(0x190));
    writer.Write(F(0x1B0));
    writer.Write(F(0x1BC));
    writer.Write(R(0x1C0));
    writer.Write(F(0x1E0));
    writer.Write(F(0x1E4));
    writer.Write(F(0x1E8));
    writer.Write(F(0x1EC));
    writer.Write(F(0x200));
    writer.Write(F(0x204));
    writer.Write(F(0x210));
    writer.Write(R(0x214));
    writer.Write(F(0x234));
    writer.Write(F(0x238));
    writer.Write(I(0x23C));
    writer.Write(F(0x240));
    writer.Write(F(0x244));
    writer.Write(F(0x248));
    writer.Write(R(0x24C));
    writer.Write(R(0x250));
    writer.Write(F(0x254));
}

static void WriteSkeletonBinary(BinaryWriter writer, HSD_JOBJ joint, int parent, List<HSD_JOBJ> joints)
{
    int index = joints.Count;
    joints.Add(joint);
    writer.Write(parent);
    WriteString(writer, string.IsNullOrWhiteSpace(joint.ClassName) ? $"JOBJ_{index}" : joint.ClassName);
    writer.Write((uint)joint.Flags);
    WriteVec3(writer, joint.TX, joint.TY, joint.TZ);
    WriteVec3(writer, joint.RX, joint.RY, joint.RZ);
    WriteVec3(writer, joint.SX, joint.SY, joint.SZ);

    HSD_JOBJ? child = joint.Child;
    while (child is not null)
    {
        WriteSkeletonBinary(writer, child, index, joints);
        child = child.Next;
    }
}

static void AddPoseJoints(HSD_JOBJ joint, List<HSD_JOBJ> joints)
{
    joints.Add(joint);
    HSD_JOBJ? child = joint.Child;
    while (child is not null)
    {
        AddPoseJoints(child, joints);
        child = child.Next;
    }
}

static void WritePoseBinary(BinaryWriter writer, HSD_JOBJ poseRoot)
{
    List<HSD_JOBJ> joints = new();
    AddPoseJoints(poseRoot, joints);
    writer.Write(joints.Count);
    foreach (HSD_JOBJ joint in joints)
    {
        WriteVec3(writer, joint.TX, joint.TY, joint.TZ);
        WriteVec3(writer, joint.RX, joint.RY, joint.RZ);
        WriteVec3(writer, joint.SX, joint.SY, joint.SZ);
    }
}

static void WriteMatrix4x4(BinaryWriter writer, float[] matrix)
{
    for (int i = 0; i < 16; ++i)
    {
        writer.Write(i < matrix.Length ? matrix[i] : (i % 5 == 0 ? 1.0f : 0.0f));
    }
}

static float[] HsdInverseBindToRowMajor(HSD_Matrix4x3? matrix)
{
    if (matrix is null)
    {
        return new[]
        {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
    }

    // HSD stores the 3x4 transform as rows used by HSD_MtxPosition.
    return new[]
    {
        matrix.M11, matrix.M12, matrix.M13, matrix.M14,
        matrix.M21, matrix.M22, matrix.M23, matrix.M24,
        matrix.M31, matrix.M32, matrix.M33, matrix.M34,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
}

static List<GX_Vertex> Triangulate(GXPrimitiveType type, List<GX_Vertex> input)
{
    List<GX_Vertex> output = new();
    switch (type)
    {
        case GXPrimitiveType.Quads:
            for (int i = 0; i + 3 < input.Count; i += 4)
            {
                output.Add(input[i]);
                output.Add(input[i + 1]);
                output.Add(input[i + 2]);
                output.Add(input[i + 2]);
                output.Add(input[i + 3]);
                output.Add(input[i]);
            }
            break;
        case GXPrimitiveType.TriangleStrip:
            for (int i = 2; i < input.Count; ++i)
            {
                bool even = i % 2 != 1;
                GX_Vertex a = input[i - 2];
                GX_Vertex b = even ? input[i] : input[i - 1];
                GX_Vertex c = even ? input[i - 1] : input[i];
                if (a != b && b != c && c != a)
                {
                    output.Add(c);
                    output.Add(b);
                    output.Add(a);
                }
            }
            break;
        case GXPrimitiveType.Triangles:
            output.AddRange(input);
            break;
    }
    return output;
}

static void WriteColor(BinaryWriter writer, byte r, byte g, byte b, byte a)
{
    writer.Write(r);
    writer.Write(g);
    writer.Write(b);
    writer.Write(a);
}

static (byte R, byte G, byte B, byte A) MaterialDiffuse(HSD_MOBJ? mobj)
{
    HSD_Material? material = mobj?.Material;
    if (material is null)
    {
        return (255, 255, 255, 255);
    }
    byte alpha = material.DIF_A != 0 ? material.DIF_A : (byte)Math.Clamp((int)MathF.Round(material.Alpha * 255.0f), 0, 255);
    if (alpha == 0)
    {
        alpha = 255;
    }
    return (material.DIF_R, material.DIF_G, material.DIF_B, alpha);
}

static bool TextureSlotEnabled(HSD_MOBJ? mobj, int slot)
{
    if (mobj is null || slot < 0 || slot >= 8)
    {
        return false;
    }
    return mobj.RenderFlags.HasFlag(RENDER_MODE.DF_ALL) || mobj.RenderFlags.HasFlag((RENDER_MODE)(1 << (slot + 4)));
}

static HSD_TOBJ? SelectDiffuseTexture(HSD_MOBJ? mobj)
{
    List<HSD_TOBJ>? textureList = mobj?.Textures?.List;
    if (textureList is null || textureList.Count == 0)
    {
        return null;
    }

    for (int i = 0; i < textureList.Count; ++i)
    {
        HSD_TOBJ texture = textureList[i];
        if (texture.ImageData is not null && TextureSlotEnabled(mobj, i) && texture.Flags.HasFlag(TOBJ_FLAGS.LIGHTMAP_DIFFUSE))
        {
            return texture;
        }
    }

    for (int i = 0; i < textureList.Count; ++i)
    {
        HSD_TOBJ texture = textureList[i];
        if (texture.ImageData is not null && TextureSlotEnabled(mobj, i))
        {
            return texture;
        }
    }

    return textureList.FirstOrDefault(t => t.ImageData is not null);
}

static (float U, float V) TransformTextureCoords(GXVector2 uv, HSD_TOBJ? texture)
{
    if (texture is null)
    {
        return (uv.X, uv.Y);
    }

    float scaleU = Math.Abs(texture.SX) < float.Epsilon ? 0.0f : texture.RepeatS / texture.SX;
    float scaleV = Math.Abs(texture.SY) < float.Epsilon ? 0.0f : texture.RepeatT / texture.SY;
    float u = uv.X * scaleU;
    float v = uv.Y * scaleV;

    float rotation = -texture.RZ;
    float c = MathF.Cos(rotation);
    float s = MathF.Sin(rotation);
    float rotatedU = u * c - v * s;
    float rotatedV = u * s + v * c;

    float mirrorOffsetV = texture.WrapT == GXWrapMode.MIRROR && Math.Abs(texture.RepeatT / texture.SY) > float.Epsilon
        ? 1.0f / (texture.RepeatT / texture.SY)
        : 0.0f;
    return (rotatedU - texture.TX, rotatedV - (texture.TY + mirrorOffsetV));
}

static int RegisterTexture(HSD_TOBJ? texture, Dictionary<string, int> textureLookup, List<(int Width, int Height, byte[] Rgba)> textures)
{
    if (texture?.ImageData is null)
    {
        return -1;
    }

    byte[]? rgba = texture.GetDecodedImageData();
    if (rgba is null || rgba.Length == 0)
    {
        return -1;
    }
    for (int i = 0; i + 3 < rgba.Length; i += 4)
    {
        (rgba[i], rgba[i + 2]) = (rgba[i + 2], rgba[i]);
    }

    string key = $"{texture.ImageData.Width}x{texture.ImageData.Height}:{Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(rgba))}";
    if (textureLookup.TryGetValue(key, out int existing))
    {
        return existing;
    }

    int index = textures.Count;
    textures.Add((texture.ImageData.Width, texture.ImageData.Height, rgba));
    textureLookup.Add(key, index);
    return index;
}

static Dictionary<HSD_JOBJ, HSD_JOBJ?> BuildParentLookup(List<HSD_JOBJ> joints)
{
    Dictionary<HSD_JOBJ, HSD_JOBJ?> parents = new();
    foreach (HSD_JOBJ joint in joints)
    {
        if (!parents.ContainsKey(joint))
        {
            parents[joint] = null;
        }
        HSD_JOBJ? child = joint.Child;
        while (child is not null)
        {
            parents[child] = joint;
            child = child.Next;
        }
    }
    return parents;
}

static int JObjIndex(Dictionary<HSD_JOBJ, int> boneIndex, HSD_JOBJ? joint)
{
    return joint is not null && boneIndex.TryGetValue(joint, out int index) ? index : -1;
}

static bool ShouldExportDObj(HSD_JOBJ parent, HSD_DOBJ dobj)
{
    if (dobj.Mobj is null)
    {
        return false;
    }
    bool materialXlu = dobj.Mobj.RenderFlags.HasFlag(RENDER_MODE.XLU);
    if (!materialXlu && parent.Flags.HasFlag(JOBJ_FLAG.OPA))
    {
        return true;
    }
    return materialXlu && (parent.Flags.HasFlag(JOBJ_FLAG.XLU) || parent.Flags.HasFlag(JOBJ_FLAG.TEXEDGE));
}

static void AddVisibilityTableEntries(
    Dictionary<int, (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable)> visibility,
    HSDArrayAccessor<SBM_LookupTable>? lookupArray,
    int modelCount,
    bool actionControlled)
{
    if (lookupArray is null)
    {
        return;
    }

    int count = Math.Min(modelCount, lookupArray.Length);
    for (int modelIndex = 0; modelIndex < count; ++modelIndex)
    {
        SBM_LookupTable? table = lookupArray[modelIndex];
        HSDArrayAccessor<SBM_LookupEntry>? entries = table?.LookupEntries;
        if (entries is null)
        {
            continue;
        }

        int stateCount = Math.Min(table?.Count ?? 0, entries.Length);
        for (int stateIndex = 0; stateIndex < stateCount; ++stateIndex)
        {
            SBM_LookupEntry? entry = entries[stateIndex];
            byte[] indexes = entry?.Entries ?? Array.Empty<byte>();
            if (indexes.Length == 0)
            {
                continue;
            }

            foreach (byte dobjIndex in indexes)
            {
                if (actionControlled)
                {
                    visibility[dobjIndex] = (modelIndex, stateIndex, true);
                }
                else if (!visibility.ContainsKey(dobjIndex))
                {
                    visibility[dobjIndex] = (-1, -1, true);
                }
            }
        }
    }
}

static Dictionary<int, (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable)> BuildDObjVisibilityTable(SBM_FighterData fighterData)
{
    Dictionary<int, (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable)> visibility = new();
    SBM_PlayerModelLookupTables? modelLookup = fighterData.ModelLookupTables;
    HSDArrayAccessor<SBM_CostumeLookupTable>? costumeLookups = modelLookup?.CostumeVisibilityLookups;
    if (modelLookup is null || costumeLookups is null || costumeLookups.Length == 0)
    {
        return visibility;
    }

    SBM_CostumeLookupTable? costume = costumeLookups[0];
    if (costume is null)
    {
        return visibility;
    }

    int modelCount = modelLookup.VisibilityLookupLength;
    AddVisibilityTableEntries(visibility, costume.HighPoly, modelCount, true);
    AddVisibilityTableEntries(visibility, costume.LowPoly, modelCount, false);
    AddVisibilityTableEntries(visibility, costume.MetalPoly, modelCount, false);
    AddVisibilityTableEntries(visibility, costume.MetalMainModel, modelCount, false);

    return visibility;
}

static void WriteMeshBinary(BinaryWriter writer, SBM_FighterData fighterData, HSD_JOBJ skeletonRoot, List<HSD_JOBJ> joints)
{
    Dictionary<HSD_JOBJ, int> boneIndex = joints.Select((joint, index) => (joint, index)).ToDictionary(item => item.joint, item => item.index);
    Dictionary<HSD_JOBJ, HSD_JOBJ?> parents = BuildParentLookup(joints);
    Dictionary<int, (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable)> visibility = BuildDObjVisibilityTable(fighterData);
    Dictionary<string, int> textureLookup = new();
    List<(int Width, int Height, byte[] Rgba)> textures = new();

    using MemoryStream batchStream = new();
    using BinaryWriter batchWriter = new(batchStream, Encoding.UTF8, leaveOpen: true);
    int batchCount = 0;

    int globalDObjIndex = 0;
    foreach (HSD_JOBJ parent in joints)
    {
        if (parent.Dobj is null)
        {
            continue;
        }

        foreach (HSD_DOBJ dobj in parent.Dobj.List)
        {
            if (dobj.Pobj is null || !ShouldExportDObj(parent, dobj))
            {
                globalDObjIndex++;
                continue;
            }

            HSD_TOBJ? selectedTexture = SelectDiffuseTexture(dobj.Mobj);
            int textureIndex = RegisterTexture(selectedTexture, textureLookup, textures);
            (byte materialR, byte materialG, byte materialB, byte materialA) = MaterialDiffuse(dobj.Mobj);
            (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable) dobjVisibility = visibility.TryGetValue(globalDObjIndex, out (int ModelPartIndex, int ModelPartState, bool HiddenByVisibilityTable) entry)
                ? entry
                : (-1, -1, false);

            foreach (HSD_POBJ pobj in dobj.Pobj.List)
            {
                if (pobj.Attributes is null)
                {
                    continue;
                }

                GX_Attribute[] attrs = pobj.ToGXAttributes();
                if (attrs.Length == 0 || attrs[^1].AttributeName != GXAttribName.GX_VA_NULL)
                {
                    continue;
                }

                GX_DisplayList displayList = pobj.ToDisplayList(attrs);
                HSD_Envelope[] envelopes = pobj.EnvelopeWeights ?? Array.Empty<HSD_Envelope>();
                bool hasMatrixIndex = pobj.HasAttribute(GXAttribName.GX_VA_PNMTXIDX);
                bool hasVertexColor = pobj.HasAttribute(GXAttribName.GX_VA_CLR0);
                List<GX_Vertex> triangles = new();
                int offset = 0;
                foreach (GX_PrimitiveGroup primitive in displayList.Primitives)
                {
                    List<GX_Vertex> vertices = displayList.Vertices.GetRange(offset, primitive.Count);
                    offset += primitive.Count;
                    triangles.AddRange(Triangulate(primitive.PrimitiveType, vertices));
                }

                if (triangles.Count == 0)
                {
                    continue;
                }

                int parentBone = JObjIndex(boneIndex, parent);
                int singleBindBone = JObjIndex(boneIndex, pobj.SingleBoundJOBJ) is int single && single >= 0 ? single : parentBone;
                batchWriter.Write(parentBone);
                batchWriter.Write(singleBindBone);
                batchWriter.Write(globalDObjIndex);
                batchWriter.Write(dobjVisibility.ModelPartIndex);
                batchWriter.Write(dobjVisibility.ModelPartState);
                batchWriter.Write(dobjVisibility.HiddenByVisibilityTable);
                batchWriter.Write((uint)parent.Flags);
                batchWriter.Write((uint)pobj.Flags);
                batchWriter.Write(hasMatrixIndex && !pobj.Flags.HasFlag(POBJ_FLAG.UNKNOWN2));
                batchWriter.Write(pobj.Flags.HasFlag(POBJ_FLAG.UNKNOWN2));
                batchWriter.Write(pobj.Flags.HasFlag(POBJ_FLAG.SHAPESET_AVERAGE));
                batchWriter.Write(textureIndex);
                WriteColor(batchWriter, materialR, materialG, materialB, materialA);
                batchWriter.Write(triangles.Count);

                foreach (GX_Vertex vertex in triangles)
                {
                    WriteVec3(batchWriter, vertex.POS.X, vertex.POS.Y, vertex.POS.Z);
                    WriteVec3(batchWriter, vertex.NRM.X, vertex.NRM.Y, vertex.NRM.Z);
                    (float u, float v) = TransformTextureCoords(vertex.TEX0, selectedTexture);
                    batchWriter.Write(u);
                    batchWriter.Write(v);
                    if (hasVertexColor)
                    {
                        WriteColor(batchWriter,
                            (byte)Math.Clamp((int)MathF.Round(vertex.CLR0.R * 255.0f), 0, 255),
                            (byte)Math.Clamp((int)MathF.Round(vertex.CLR0.G * 255.0f), 0, 255),
                            (byte)Math.Clamp((int)MathF.Round(vertex.CLR0.B * 255.0f), 0, 255),
                            (byte)Math.Clamp((int)MathF.Round(vertex.CLR0.A * 255.0f), 0, 255));
                    }
                    else
                    {
                        WriteColor(batchWriter, 255, 255, 255, 255);
                    }

                    (int Bone, float Weight)[] influences = new (int Bone, float Weight)[6];
                    for (int i = 0; i < influences.Length; ++i)
                    {
                        influences[i] = (-1, 0.0f);
                    }

                    if (pobj.Flags.HasFlag(POBJ_FLAG.UNKNOWN2))
                    {
                        int matrixIndex = vertex.PNMTXIDX / 3;
                        HSD_JOBJ? influenceJoint = matrixIndex == 1 && parents.TryGetValue(parent, out HSD_JOBJ? grandparent) ? grandparent : parent;
                        influences[0] = (JObjIndex(boneIndex, influenceJoint), 1.0f);
                    }
                    else if (hasMatrixIndex && envelopes.Length > 0)
                    {
                        int envelopeIndex = vertex.PNMTXIDX / 3;
                        if (envelopeIndex >= 0 && envelopeIndex < envelopes.Length)
                        {
                            HSD_Envelope envelope = envelopes[envelopeIndex];
                            int count = Math.Min(envelope.EnvelopeCount, influences.Length);
                            for (int i = 0; i < count; ++i)
                            {
                                influences[i] = (JObjIndex(boneIndex, envelope.GetJOBJAt(i)), envelope.GetWeightAt(i));
                            }
                        }
                    }

                    for (int i = 0; i < influences.Length; ++i)
                    {
                        batchWriter.Write(influences[i].Bone);
                        batchWriter.Write(influences[i].Weight);
                    }
                }

                batchCount++;
            }
            globalDObjIndex++;
        }
    }

    writer.Write(joints.Count);
    foreach (HSD_JOBJ joint in joints)
    {
        WriteMatrix4x4(writer, HsdInverseBindToRowMajor(joint.InverseWorldTransform));
    }

    writer.Write(textures.Count);
    foreach ((int width, int height, byte[] rgba) in textures)
    {
        writer.Write(width);
        writer.Write(height);
        writer.Write(rgba.Length);
        writer.Write(rgba);
    }

    writer.Write(batchCount);
    writer.Write(batchStream.ToArray());
}

static List<FOBJKey> SampleKeys(FOBJ_Player player, float frameCount, JointTrackType trackType)
{
    int lastFrame = Math.Max(1, (int)MathF.Ceiling(frameCount));
    List<FOBJKey> sampledKeys = new();
    float previous = 0.0f;
    for (int frame = 0; frame <= lastFrame; ++frame)
    {
        float value = player.GetValue(frame);
        if (sampledKeys.Count > 0 && IsRotationTrack(trackType))
        {
            float delta = value - previous;
            while (delta > MathF.PI)
            {
                value -= MathF.PI * 2.0f;
                delta -= MathF.PI * 2.0f;
            }
            while (delta < -MathF.PI)
            {
                value += MathF.PI * 2.0f;
                delta += MathF.PI * 2.0f;
            }
        }
        sampledKeys.Add(new FOBJKey
        {
            Frame = frame,
            Value = value,
            Tan = 0.0f,
            InterpolationType = GXInterpolationType.HSD_A_OP_LIN,
        });
        previous = value;
    }
    return sampledKeys;
}

static void WriteAnimationClipBinary(BinaryWriter writer, string name, int actionIndex, uint flags, byte defaultBlendFrames, float frameCount, List<(int Joint, JointTrackType TrackType, List<FOBJKey> Keys)> tracks)
{
    WriteString(writer, name);
    writer.Write(actionIndex);
    writer.Write(flags);
    writer.Write(defaultBlendFrames);
    writer.Write(frameCount);

    writer.Write(tracks.Count);
    foreach ((int joint, JointTrackType trackType, List<FOBJKey> keys) in tracks)
    {
        writer.Write(joint);
        writer.Write(TrackId(trackType));
        writer.Write(keys.Count);
        foreach (FOBJKey key in keys)
        {
            writer.Write(key.Frame);
            writer.Write(key.Value);
            writer.Write(key.Tan);
            writer.Write(InterpolationId(key.InterpolationType));
        }
    }
}

static void WriteClipBinary(BinaryWriter writer, string name, int actionIndex, uint flags, byte defaultBlendFrames, int targetFighterKind, SBM_ftLoadCommonData? commonData, HSD_FigaTree clip)
{
    List<(int Joint, JointTrackType TrackType, List<FOBJKey> Keys)> tracks = new();
    List<FigaTreeNode> nodes = clip.Nodes;
    for (int nodeIndex = 0; nodeIndex < nodes.Count; ++nodeIndex)
    {
        int targetJoint = RemapFigaNodeToTargetJoint(commonData, flags, targetFighterKind, nodeIndex);
        if (targetJoint < 0)
        {
            continue;
        }
        foreach (HSD_Track track in nodes[nodeIndex].Tracks)
        {
            byte trackId = TrackId(track.JointTrackType);
            if (trackId == 255)
            {
                continue;
            }
            FOBJ_Player player = new(track.ToFOBJ());
            if (player.Keys.Count == 0)
            {
                continue;
            }
            tracks.Add((targetJoint, track.JointTrackType, SampleKeys(player, clip.FrameCount, track.JointTrackType)));
        }
    }
    WriteAnimationClipBinary(writer, name, actionIndex, flags, defaultBlendFrames, clip.FrameCount, tracks);
}

static int MeleeCommandByteSize(byte code)
{
    // Melee's fighter hitbox command is five 32-bit command words. HSDLib's generic
    // ActionCommon map predates the decomp layout and truncates this command.
    if (code == 0x0B)
    {
        return 20;
    }
    if (code == 0x22)
    {
        return 12;
    }
    if (code == 0x38)
    {
        return 8;
    }
    MeleeCMDAction? action = ActionCommon.GetMeleeCMDAction(code);
    return action?.ByteSize ?? 4;
}

static List<(byte Code, byte[] Bytes)> ReadActionCommandsFromStruct(HSDStruct data, HashSet<HSDStruct> callStack)
{
    List<(byte Code, byte[] Bytes)> commands = new();
    if (!callStack.Add(data))
    {
        return commands;
    }

    byte[] bytes = data.GetData();
    int offset = 0;
    while (offset < bytes.Length)
    {
        byte code = (byte)(bytes[offset] >> 2);
        int byteSize = MeleeCommandByteSize(code);
        if (byteSize <= 0 || offset + byteSize > bytes.Length)
        {
            break;
        }

        byte[] commandBytes = bytes.Skip(offset).Take(byteSize).ToArray();
        int nextOffset = offset + byteSize;
        if (code == 0x05 || code == 0x07)
        {
            if (data.References.TryGetValue(nextOffset - 4, out HSDStruct? target))
            {
                commands.AddRange(ReadActionCommandsFromStruct(target, callStack));
            }
            if (code == 0x07)
            {
                break;
            }
            offset = nextOffset;
            continue;
        }

        commands.Add((code, commandBytes));
        offset += byteSize;
        if (code == 0)
        {
            break;
        }
    }
    callStack.Remove(data);
    return commands;
}

static List<(byte Code, byte[] Bytes)> ReadActionCommands(SBM_FighterSubactionData? subaction)
{
    return subaction is null ? new() : ReadActionCommandsFromStruct(subaction._s, new HashSet<HSDStruct>());
}

static void WriteActionScriptBinary(BinaryWriter writer, SBM_FighterAction action, int actionIndex, int[] commonBoneLookup)
{
    WriteString(writer, action.Name ?? "");
    writer.Write(actionIndex);
    writer.Write(commonBoneLookup.Length);
    foreach (int bone in commonBoneLookup)
    {
        writer.Write(bone);
    }
    List<(byte Code, byte[] Bytes)> commands = ReadActionCommands(action.SubAction);
    writer.Write(commands.Count);
    foreach ((byte code, byte[] bytes) in commands)
    {
        writer.Write(code);
        writer.Write(bytes.Length);
        writer.Write(bytes);
    }
}

static void ExportFighterAssetBinary(string outputPath, string fighterDatPath, string costumeDatPath)
{
    HSDRawFile fighterFile = new(fighterDatPath);
    HSDRawFile costumeFile = new(costumeDatPath);
    SBM_FighterData fighterData = fighterFile.Roots.Select(root => root.Data).OfType<SBM_FighterData>().First();
    HSD_JOBJ skeletonRoot = costumeFile.Roots.Select(root => root.Data).OfType<HSD_JOBJ>().First();
    AnimationDatSet? animationSet = TryLoadAnimationDatSet(fighterDatPath);
    SBM_ftLoadCommonData? commonData = TryLoadCommonDataForFighter(fighterDatPath);
    SBM_DynamicBehavior[] dynamicBehaviors = fighterData.FighterActionDynamicBehaviors?.Array ?? Array.Empty<SBM_DynamicBehavior>();

    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputPath)) ?? ".");
    using FileStream stream = File.Create(outputPath);
    using BinaryWriter writer = new(stream, Encoding.UTF8);

    writer.Write(Encoding.ASCII.GetBytes("PFHA"));
    WriteString(writer, Path.GetFileNameWithoutExtension(fighterDatPath));

    List<HSD_JOBJ> joints = new();
    using MemoryStream skeletonStream = new();
    using (BinaryWriter skeletonWriter = new(skeletonStream, Encoding.UTF8, leaveOpen: true))
    {
        WriteSkeletonBinary(skeletonWriter, skeletonRoot, -1, joints);
        writer.Write(joints.Count);
    }
    writer.Write(skeletonStream.ToArray());

    SBM_FighterBoneIDs? bones = fighterData.FighterBoneTable;
    writer.Write(bones?.HeadBone ?? -1);
    writer.Write(bones?.RightArm ?? -1);
    writer.Write(bones?.LeftLeg ?? -1);
    writer.Write(bones?.RightLeg ?? -1);
    writer.Write(bones?.LeftArm ?? -1);
    SBM_PlayerModelLookupTables? modelLookup = fighterData.ModelLookupTables;
    writer.Write(modelLookup?.ItemHoldBone ?? -1);
    writer.Write(modelLookup?.ShieldBone ?? -1);
    writer.Write(modelLookup?.TopOfHeadBone ?? -1);
    writer.Write(modelLookup?.LeftFootBone ?? -1);
    writer.Write(modelLookup?.RightFootBone ?? -1);
    int targetFighterKind = FighterKindForDatPath(fighterDatPath);
    int[] fighterCommonBoneLookup = ReadCommonBoneLookup(commonData, targetFighterKind);
    writer.Write(fighterCommonBoneLookup.Length);
    foreach (int bone in fighterCommonBoneLookup)
    {
        writer.Write(bone);
    }
    WriteFighterAttributesBinary(writer, fighterData.Attributes);

    SBM_EnvironmentCollision? env = fighterData.EnvironmentCollision;
    writer.Write(env is not null);
    writer.Write((int)(env?.ECBBone1 ?? -1));
    writer.Write((int)(env?.ECBBone2 ?? -1));
    writer.Write((int)(env?.ECBBone3 ?? -1));
    writer.Write((int)(env?.ECBBone4 ?? -1));
    writer.Write((int)(env?.ECBBone5 ?? -1));
    writer.Write((int)(env?.ECBBone6 ?? -1));
    writer.Write(env?.Multiplier ?? 0.0f);
    writer.Write(env?.LedgeGrabWidth ?? 0.0f);
    writer.Write(env?.LedgeGrabYOffset ?? 0.0f);
    writer.Write(env?.LedgeGrabHeight ?? 0.0f);

    SBM_Hurtbox[] hurtboxes = fighterData.Hurtboxes?.Hurtboxes ?? Array.Empty<SBM_Hurtbox>();
    writer.Write(hurtboxes.Length);
    for (int i = 0; i < hurtboxes.Length; ++i)
    {
        SBM_Hurtbox hurtbox = hurtboxes[i];
        writer.Write(i);
        writer.Write(hurtbox.BoneIndex);
        WriteString(writer, hurtbox.Type.ToString());
        writer.Write(hurtbox.Grabbable != 0);
        WriteVec3(writer, hurtbox.X1, hurtbox.Y1, hurtbox.Z1);
        WriteVec3(writer, hurtbox.X2, hurtbox.Y2, hurtbox.Z2);
        writer.Write(hurtbox.Size);
    }

    WriteMeshBinary(writer, fighterData, skeletonRoot, joints);

    SBM_ModelPart[] modelParts = fighterData.ModelPartAnimations?.Array ?? Array.Empty<SBM_ModelPart>();
    writer.Write(modelParts.Length);
    for (int partIndex = 0; partIndex < modelParts.Length; ++partIndex)
    {
        SBM_ModelPart part = modelParts[partIndex];
        byte[] entries = part.Entries ?? Array.Empty<byte>();
        writer.Write((int)part.StartingBone);
        writer.Write(entries.Length);
        foreach (byte entry in entries)
        {
            writer.Write((int)entry);
        }

        HSD_AnimJoint[] animations = part.Anims?.Array ?? Array.Empty<HSD_AnimJoint>();
        writer.Write(animations.Length);
        for (int animIndex = 0; animIndex < animations.Length; ++animIndex)
        {
            HSD_AnimJoint animation = animations[animIndex];
            HSD_AnimJoint[] nodes = animation?.TreeList?.ToArray() ?? Array.Empty<HSD_AnimJoint>();
            List<(int Joint, JointTrackType TrackType, List<FOBJKey> Keys)> tracks = new();
            float frameCount = 1.0f;
            foreach (byte entry in entries)
            {
                int localIndex = entry - part.StartingBone;
                if (localIndex < 0 || localIndex >= nodes.Length)
                {
                    continue;
                }
                HSD_AnimJoint joint = nodes[localIndex];
                if (joint.AOBJ is null || joint.AOBJ.FObjDesc is null)
                {
                    continue;
                }
                frameCount = MathF.Max(frameCount, joint.AOBJ.EndFrame + 1.0f);
                foreach (HSD_FOBJDesc fdesc in joint.AOBJ.FObjDesc.List)
                {
                    FOBJ_Player player = new(fdesc);
                    if (player.Keys is null || player.Keys.Count == 0)
                    {
                        continue;
                    }
                    JointTrackType trackType = player.JointTrackType;
                    if (TrackId(trackType) == 255)
                    {
                        continue;
                    }
                    frameCount = MathF.Max(frameCount, player.Keys.Max(key => key.Frame + 1.0f));
                    tracks.Add((entry, trackType, SampleKeys(player, frameCount, trackType)));
                }
            }
            WriteAnimationClipBinary(writer, $"model_part_{partIndex}_{animIndex}", -1, 0, 0, frameCount, tracks);
        }
    }

    List<(string Name, int Index, uint Flags, byte DefaultBlendFrames, HSD_FigaTree Clip)> clips = new();
    if (fighterData.FighterActionTable is not null)
    {
        SBM_FighterAction[] actions = fighterData.FighterActionTable.Commands;
        for (int i = 0; i < actions.Length; ++i)
        {
            SBM_FighterAction action = actions[i];
            if (action.AnimationSize <= 0 || string.IsNullOrWhiteSpace(action.Name))
            {
                continue;
            }
            HSD_FigaTree? clip = FigaForAction(animationSet, action);
            if (clip is not null)
            {
                byte defaultBlendFrames = i < dynamicBehaviors.Length ? dynamicBehaviors[i].Flags : (byte)0;
                clips.Add((action.Name, i, action.Flags, defaultBlendFrames, clip));
            }
        }
    }
    writer.Write(clips.Count);
    foreach ((string name, int index, uint flags, byte defaultBlendFrames, HSD_FigaTree clip) in clips)
    {
        WriteClipBinary(writer, name, index, flags, defaultBlendFrames, targetFighterKind, commonData, clip);
    }

    List<(SBM_FighterAction Action, int Index, int[] CommonBoneLookup)> scripts = new();
    if (fighterData.FighterActionTable is not null)
    {
        SBM_FighterAction[] actions = fighterData.FighterActionTable.Commands;
        for (int i = 0; i < actions.Length; ++i)
        {
            if (actions[i].SubAction is not null)
            {
                int boneTableIndex = i < dynamicBehaviors.Length ? dynamicBehaviors[i].BoneTableIndex : -1;
                scripts.Add((actions[i], i, ReadCommonBoneLookup(commonData, boneTableIndex)));
            }
        }
    }
    writer.Write(scripts.Count);
    foreach ((SBM_FighterAction action, int index, int[] commonBoneLookup) in scripts)
    {
        WriteActionScriptBinary(writer, action, index, commonBoneLookup);
    }

    HSD_JOBJ? shieldPose = fighterData.ShieldPoseContainer?.ShieldPose;
    writer.Write(shieldPose is not null);
    if (shieldPose is not null)
    {
        WritePoseBinary(writer, shieldPose);
    }
}

static byte StageLineKindId(CollPhysics physics)
{
    if (physics.HasFlag(CollPhysics.Top)) return 0;
    if (physics.HasFlag(CollPhysics.Bottom)) return 1;
    if (physics.HasFlag(CollPhysics.Right)) return 2;
    if (physics.HasFlag(CollPhysics.Left)) return 3;
    return 0;
}

static void ExportStageCollisionBinary(string outputPath, string stageDatPath)
{
    HSDRawFile stageFile = new(stageDatPath);
    SBM_Coll_Data collision = stageFile.Roots.Select(root => root.Data).OfType<SBM_Coll_Data>().First();
    SBM_GroundParam? groundParam = stageFile.Roots.Select(root => root.Data).OfType<SBM_GroundParam>().FirstOrDefault();
    float stageScale = groundParam?.StageScale ?? 1.0f;
    if (stageScale == 0.0f)
    {
        stageScale = 1.0f;
    }
    SBM_CollVertex[] vertices = collision.Vertices ?? Array.Empty<SBM_CollVertex>();
    SBM_CollLine[] lines = collision.Links ?? Array.Empty<SBM_CollLine>();

    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputPath)) ?? ".");
    using FileStream stream = File.Create(outputPath);
    using BinaryWriter writer = new(stream, Encoding.UTF8);

    writer.Write(Encoding.ASCII.GetBytes("PFST"));
    WriteString(writer, Path.GetFileNameWithoutExtension(stageDatPath));

    List<(SBM_CollLine Line, int OriginalIndex)> exported = lines
        .Select((line, index) => (Line: line, OriginalIndex: index))
        .Where(item => !item.Line.CollisionFlag.HasFlag(CollPhysics.Disabled))
        .Where(item => item.Line.VertexIndex1 >= 0 && item.Line.VertexIndex1 < vertices.Length && item.Line.VertexIndex2 >= 0 && item.Line.VertexIndex2 < vertices.Length)
        .ToList();
    Dictionary<int, int> exportedIndexByOriginal = exported
        .Select((item, exportedIndex) => (item.OriginalIndex, exportedIndex))
        .ToDictionary(item => item.OriginalIndex, item => item.exportedIndex);

    int RemapLineIndex(short originalIndex)
    {
        return exportedIndexByOriginal.TryGetValue(originalIndex, out int exportedIndex) ? exportedIndex : -1;
    }

    writer.Write(exported.Count);
    foreach ((SBM_CollLine line, int _) in exported)
    {
        SBM_CollVertex a = vertices[line.VertexIndex1];
        SBM_CollVertex b = vertices[line.VertexIndex2];
        writer.Write(a.X * stageScale);
        writer.Write(a.Y * stageScale);
        writer.Write(b.X * stageScale);
        writer.Write(b.Y * stageScale);
        writer.Write(line.Flag.HasFlag(CollProperty.DropThrough) ? (byte)1 : (byte)0);
        writer.Write(StageLineKindId(line.CollisionFlag));
        bool ledgeGrab = line.Flag.HasFlag(CollProperty.LedgeGrab);
        writer.Write(ledgeGrab);
        writer.Write(ledgeGrab);
        writer.Write(false);
        writer.Write(RemapLineIndex(line.NextLine));
        writer.Write(RemapLineIndex(line.PreviousLine));
        writer.Write(1.0f);
    }
}

static AnimationDatSet? TryLoadAnimationDatSet(string datPath)
{
    string? directory = Path.GetDirectoryName(datPath);
    string baseName = Path.GetFileNameWithoutExtension(datPath);
    string ajPath = Path.Combine(directory ?? "", $"{baseName}AJ.dat");
    return File.Exists(ajPath) ? new AnimationDatSet(ajPath) : null;
}

static HSD_FigaTree? FigaForAction(AnimationDatSet? animationSet, SBM_FighterAction action)
{
    return animationSet?.GetClip(action.Name, action.AnimationOffset);
}

if (args.Length == 0)
{
    Console.Error.WriteLine("Usage:");
    Console.Error.WriteLine("  dotnet run --project engine_cpp/tools/hsd_exporter -- --asset-bin-out <output.bin> <fighter.dat> <costume.dat>");
    Console.Error.WriteLine("  dotnet run --project engine_cpp/tools/hsd_exporter -- --stage-bin-out <output.bin> <stage.dat>");
    Console.Error.WriteLine("  dotnet run --project engine_cpp/tools/hsd_exporter -- --common-bin-out <output.bin> <PlCo.dat>");
    return 2;
}

if (args[0] == "--asset-bin-out")
{
    if (args.Length != 4)
    {
        Console.Error.WriteLine("--asset-bin-out expects <output.bin> <fighter.dat> <costume.dat>");
        return 2;
    }
    ExportFighterAssetBinary(args[1], args[2], args[3]);
    Console.WriteLine(Path.GetFullPath(args[1]));
    return 0;
}

if (args[0] == "--stage-bin-out")
{
    if (args.Length != 3)
    {
        Console.Error.WriteLine("--stage-bin-out expects <output.bin> <stage.dat>");
        return 2;
    }
    ExportStageCollisionBinary(args[1], args[2]);
    Console.WriteLine(Path.GetFullPath(args[1]));
    return 0;
}

if (args[0] == "--common-bin-out")
{
    if (args.Length != 3)
    {
        Console.Error.WriteLine("--common-bin-out expects <output.bin> <PlCo.dat>");
        return 2;
    }
    WriteCommonDataBinary(args[1], args[2]);
    Console.WriteLine(Path.GetFullPath(args[1]));
    return 0;
}

Console.Error.WriteLine($"unknown exporter command: {args[0]}");
return 2;

sealed class AnimationDatSet
{
    private readonly byte[] bytes;
    private readonly FighterAJManager manager;
    private readonly Dictionary<string, HSD_FigaTree?> clipsBySymbol = new();
    private readonly Dictionary<int, HSD_FigaTree?> clipsByOffset = new();

    public AnimationDatSet(string path)
    {
        bytes = File.ReadAllBytes(path);
        manager = new FighterAJManager(bytes);
    }

    public HSD_FigaTree? GetClip(string symbol, int offset)
    {
        if (!string.IsNullOrWhiteSpace(symbol))
        {
            if (clipsBySymbol.TryGetValue(symbol, out HSD_FigaTree? cachedBySymbol))
            {
                return cachedBySymbol;
            }

            byte[] data = manager.GetAnimationData(symbol);
            if (data is not null)
            {
                try
                {
                    HSDRawFile file = new(data);
                    HSD_FigaTree? clip = file.Roots.Select(root => root.Data).OfType<HSD_FigaTree>().FirstOrDefault();
                    clipsBySymbol[symbol] = clip;
                    return clip;
                }
                catch
                {
                    clipsBySymbol[symbol] = null;
                }
            }
        }

        if (clipsByOffset.TryGetValue(offset, out HSD_FigaTree? cached))
        {
            return cached;
        }

        if (offset < 0 || offset + 4 > bytes.Length)
        {
            clipsByOffset[offset] = null;
            return null;
        }

        try
        {
            int fileSize =
                (bytes[offset] << 24) |
                (bytes[offset + 1] << 16) |
                (bytes[offset + 2] << 8) |
                bytes[offset + 3];
            if (fileSize <= 0 || offset + fileSize > bytes.Length)
            {
                clipsByOffset[offset] = null;
                return null;
            }
            byte[] chunk = bytes[offset..(offset + fileSize)];
            HSDRawFile file = new(chunk);
            HSD_FigaTree? clip = file.Roots.Select(root => root.Data).OfType<HSD_FigaTree>().FirstOrDefault();
            if (clip is null && file.Roots.Count > 0)
            {
                clip = new HSD_FigaTree { _s = file.Roots[0].Data._s };
            }
            clipsByOffset[offset] = clip;
            return clip;
        }
        catch
        {
            clipsByOffset[offset] = null;
            return null;
        }
    }
}
