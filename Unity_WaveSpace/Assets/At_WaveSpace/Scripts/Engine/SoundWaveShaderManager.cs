using System.Collections.Generic;
using UnityEngine;
using System.Linq;

/// <summary>
/// Drives the WFS wavefront compute shader and assigns the result to the plane's
/// material.  Works with Built-in RP, URP, and HDRP because it never relies on
/// OnRenderImage (a Built-in-only camera callback).
///
/// Setup:
///   1. Attach this script to the WavefrontDisplayPlanePrefab (the plane).
///   2. Assign the ComputeShader (SoundWaveShader.compute) in the Inspector.
///   3. The plane's material must use the  AT_WaveSpace/SoundwaveUnlit  shader.
///      The script will assign the runtime RenderTexture to _MainTex automatically.
///   4. Call Init() after all audio objects are ready (e.g. at the end of
///      At_MasterOutput.Awake()).
/// </summary>
[RequireComponent(typeof(Renderer))]
public class SoundWaveShaderManager : MonoBehaviour
{
    // ─── Inspector ────────────────────────────────────────────────────────────
    public ComputeShader _computeShader;

    [Tooltip("Resolution of the generated RenderTexture (pixels per side).")]
    public int renderTextureResolution = 1024;

    public At_MasterOutput master;
    public At_Player        player;
    public float            _waveFrequency = 440f;

    [Tooltip("Log-compression strength for the displayed intensity. 0 = linear (original behaviour). " +
             "Raise this to keep wavefronts visible far from any speaker on large speaker rings, " +
             "where the raw 1/r² amplitude would otherwise look black almost everywhere.")]
    public float _logCompression = 0f;

    // Public for Inspector visualisation
    [HideInInspector] public Vector4[]    _wavePositions;
    [HideInInspector] public GameObject[] speakers;
    [HideInInspector] public float[]      _wfsAmps;
    [HideInInspector] public float[]      _wfsDelays;
    [HideInInspector] public float[]      _wfsSpeakerMask;

    // ─── Private ──────────────────────────────────────────────────────────────
    private Vector4[] _wfsAmps_v4;
    private Vector4[] _wfsDelays_v4;
    private Vector4[] _wfsSpeakerMask_v4;

    private ComputeBuffer _wavePositionsBuffer;
    private ComputeBuffer _wfsAmpsBuffer;
    private ComputeBuffer _wfsDelaysBuffer;
    private ComputeBuffer _wfsSpeakerMaskBuffer;

    private RenderTexture _target;
    private Material      _planeMaterial;
    private int           _kernelCSMain = -1;

    // Kept in sync with At_MasterOutput.secondarySourceSize via SetSecondarySourceSize().
    // Same value as the WFS engine (m_secondarySourceSize) for audio/visual coherence.
    private float _secondarySourceSize = 0.3f;

    // Resolved once in Init(), used by ComputeDistanceGain() each frame.
    private At_Listener _listener;

    // Distance attenuation gain, recomputed every Update() from player.transform,
    // _listener.transform, player.attenuation and player.minDistance — same formula
    // as Spatializer::computeDistanceGain() in the audio engine (see ComputeDistanceGain()
    // below), kept in C# only so no native/DLL change is needed.
    private float _distanceGain = 1f;

    private const int MAX_CHANNEL_COUNT = 1024;

    private bool _initialized = false;

    // ─── Public API ───────────────────────────────────────────────────────────

    /// <summary>
    /// Called by At_MasterOutput.Update() whenever secondarySourceSize changes.
    /// Keeps the shader epsilon coherent with the WFS audio engine parameter.
    /// </summary>
    public void SetSecondarySourceSize(float sourceSize)
    {
        _secondarySourceSize = Mathf.Max(0f, sourceSize);
        // Value is pushed to the shader each LateUpdate via SetShaderDynamicParameters()
    }

    /// <summary>
    /// Call this after all audio objects are ready, e.g. at the end of
    /// At_MasterOutput.Awake(). Safe to call multiple times (re-initialises).
    /// </summary>
    public void Init()
    {
        if (master == null) master = FindObjectOfType<At_MasterOutput>();
        if (player == null) player = Find3DPlayer();
        if (_listener == null) _listener = FindObjectOfType<At_Listener>();

        if (master == null || player == null) return;

        // ── Release previous resources if re-initialising ──
        ReleaseBuffers();
        ReleaseTexture();

        int count = master.numVirtualSpeakers;

        _wavePositions     = new Vector4[count];
        speakers           = new GameObject[count];
        _wfsAmps           = new float[count];
        _wfsDelays         = new float[count];
        _wfsSpeakerMask    = new float[count];
        _wfsAmps_v4        = new Vector4[count];
        _wfsDelays_v4      = new Vector4[count];
        _wfsSpeakerMask_v4 = new Vector4[count];

        // ── ComputeBuffers (stride = sizeof(float4) = 16 bytes) ──
        _wavePositionsBuffer  = new ComputeBuffer(MAX_CHANNEL_COUNT, sizeof(float) * 4);
        _wfsAmpsBuffer        = new ComputeBuffer(MAX_CHANNEL_COUNT, sizeof(float) * 4);
        _wfsDelaysBuffer      = new ComputeBuffer(MAX_CHANNEL_COUNT, sizeof(float) * 4);
        _wfsSpeakerMaskBuffer = new ComputeBuffer(MAX_CHANNEL_COUNT, sizeof(float) * 4);

        // ── Gather and sort virtual speakers ──
        At_VirtualSpeaker[] vss = FindObjectsOfType<At_VirtualSpeaker>();
        vss = vss.OrderBy(x => x.id).ToArray();
        for (int i = 0; i < Mathf.Min(vss.Length, count); i++)
            speakers[i] = vss[i].gameObject;

        // ── RenderTexture ──
        _target = new RenderTexture(renderTextureResolution, renderTextureResolution, 0,
            RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear)
        {
            enableRandomWrite = true,
            filterMode        = FilterMode.Bilinear,
            wrapMode          = TextureWrapMode.Clamp
        };
        _target.Create();

        // ── Assign RT to the plane's material ──
        _planeMaterial = GetComponent<Renderer>().material;
        _planeMaterial.SetTexture("_MainTex", _target);

        // ── Cache kernel index ──
        _kernelCSMain = _computeShader.FindKernel("CSMain");

        // ── Push static parameters once ──
        SetShaderStaticParameters();

        _initialized = true;
    }

    // ─── Unity Messages ───────────────────────────────────────────────────────

    /// <summary>
    /// Called once when the component first becomes active.
    /// Handles the case where the prefab is added during play mode — at that point
    /// At_MasterOutput.Awake() has already run and will not call Init() again.
    /// </summary>
    private void Start()
    {
        if (!_initialized)
            TryAutoInit();
    }

    /// <summary>
    /// Called every time the component is enabled (including after Instantiate).
    /// Re-initialises if a previous Init() produced valid results so that
    /// re-enabling the GameObject in play mode also works correctly.
    /// </summary>
    private void OnEnable()
    {
        // Only auto-init in play mode and only if Start() hasn't run yet
        // (Start() handles the first-time case; this covers re-enable).
        if (Application.isPlaying && _initialized)
        {
            // Re-sync secondary source size from master in case it changed
            // while this object was disabled.
            if (master != null)
                SetSecondarySourceSize(master.secondarySourceSize);
        }
    }

    /// <summary>
    /// Attempts to find At_MasterOutput and At_Player in the scene and call Init().
    /// Also syncs secondarySourceSize from the master immediately after init.
    /// </summary>
    private void TryAutoInit()
    {
        if (master == null) master = FindObjectOfType<At_MasterOutput>();
        if (player == null) player = Find3DPlayer();

        if (master == null || player == null) return;

        Init();

        if (_initialized)
            SetSecondarySourceSize(master.secondarySourceSize);
    }

    private void Update()
    {
        if (!_initialized) return;

        _distanceGain = ComputeDistanceGain();

        int count = master.numVirtualSpeakers;
        for (int i = 0; i < count; i++)
        {
            _wavePositions[i].x = speakers[i].transform.position.x;
            _wavePositions[i].z = speakers[i].transform.position.z;

            _wfsAmps_v4[i].x        = player.volumeArray[i];
            _wfsAmps[i]              = _wfsAmps_v4[i].x;

            _wfsDelays_v4[i].x      = player.delayArray[i];
            _wfsDelays[i]            = _wfsDelays_v4[i].x;

            _wfsSpeakerMask_v4[i].x = player.activationSpeakerVolume[i];
            _wfsSpeakerMask[i]       = _wfsSpeakerMask_v4[i].x;
        }
    }

    private void LateUpdate()
    {
        if (!_initialized) return;

        SetShaderDynamicParameters();
        DispatchCompute();
    }

    private void OnDestroy()
    {
        ReleaseBuffers();
        ReleaseTexture();
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    /// <summary>
    /// Distance attenuation gain for the source — same formula as
    /// Spatializer::computeDistanceGain() in the audio engine (AT_Spatializer.cpp):
    ///   d       = 3D Euclidean distance from source to listener
    ///   dClamped = max(d, minDistance)           // no boost below minDistance
    ///   gain     = (minDistance / dClamped) ^ attenuation
    ///   gain     = clamp(gain, 0, 1)              // never boost, never negative
    /// Computed here in C# (not read from the native engine) since every input
    /// (positions, attenuation, minDistance) is already available on the Unity side.
    /// </summary>
    private float ComputeDistanceGain()
    {
        if (_listener == null || player.attenuation <= 0f)
            return 1f;

        float d = Vector3.Distance(player.transform.position, _listener.gameObject.transform.position);
        float dClamped = Mathf.Max(d, player.minDistance);

        float gain = dClamped > 0f
            ? Mathf.Pow(player.minDistance / dClamped, player.attenuation)
            : 1f;

        return Mathf.Clamp01(gain);
    }

    /// <summary>
    /// Evaluates the wavefront signal at an arbitrary world-space XZ position, using the
    /// exact same formula as the compute shader (getValueForSinWaveAtPosition + summation +
    /// distance gain + log-compression). Lets other scripts (e.g. ambient emission on nearby
    /// meshes) sample the same visual signal as the plane without reading back the GPU texture.
    /// </summary>
    /// <returns>Signed value: positive = "blue" wavefront, negative = "red" wavefront (same convention as the shader).</returns>
    public float EvaluateWaveAt(Vector3 worldPosition)
    {
        if (!_initialized) return 0f;

        float value = 0f;
        int count = master.numVirtualSpeakers;
        for (int i = 0; i < count; i++)
        {
            float dx = worldPosition.x - _wavePositions[i].x;
            float dz = worldPosition.z - _wavePositions[i].z;
            float rRaw = Mathf.Sqrt(dx * dx + dz * dz);
            float r = Mathf.Sqrt(rRaw * rRaw + _secondarySourceSize * _secondarySourceSize);

            float wave = _wfsAmps[i] * (1f / (r * r))
                * Mathf.Sin(2f * Mathf.PI * _waveFrequency * (r / 340f + _wfsDelays[i]));

            value += _wfsSpeakerMask[i] * wave;
        }

        value *= _distanceGain;

        if (_logCompression > 0f)
        {
            float sign = value >= 0f ? 1f : -1f;
            float mag = Mathf.Abs(value);
            mag = Mathf.Log(1f + _logCompression * mag) / Mathf.Log(1f + _logCompression);
            value = sign * mag;
        }

        return value;
    }

    /// <summary>
    /// Parameters that depend only on the plane's transform.
    /// Called once at Init(); call again manually if the transform changes at runtime.
    /// </summary>
    private void SetShaderStaticParameters()
    {
        _computeShader.SetFloat("_outputChannelCount",    master.numVirtualSpeakers);
        _computeShader.SetFloat("_displayPlaneSizeX",     10f * transform.localScale.x);
        _computeShader.SetFloat("_displayPlaneSizeZ",     10f * transform.localScale.z);
        _computeShader.SetFloat("_displayPlanePositionX", transform.position.x);
        _computeShader.SetFloat("_displayPlanePositionZ", transform.position.z);
    }

    /// <summary>Upload per-frame audio data and editable parameters via ComputeBuffers.</summary>
    private void SetShaderDynamicParameters()
    {
        // _waveFrequency is here so Inspector edits take effect immediately every frame
        _computeShader.SetFloat("_waveFrequency",     _waveFrequency);
        // _secondarySourceSize synced with At_MasterOutput.secondarySourceSize (audio engine)
        _computeShader.SetFloat("_secondarySourceSize", _secondarySourceSize);
        // Distance attenuation gain, recomputed each Update() — see ComputeDistanceGain()
        _computeShader.SetFloat("_distanceGain", _distanceGain);
        // Log-compression strength for the displayed intensity (Inspector-editable)
        _computeShader.SetFloat("_logCompression", _logCompression);

        _wavePositionsBuffer.SetData(_wavePositions);
        _wfsAmpsBuffer.SetData(_wfsAmps_v4);
        _wfsDelaysBuffer.SetData(_wfsDelays_v4);
        _wfsSpeakerMaskBuffer.SetData(_wfsSpeakerMask_v4);

        _computeShader.SetBuffer(_kernelCSMain, "_wavePositions",  _wavePositionsBuffer);
        _computeShader.SetBuffer(_kernelCSMain, "_wfsAmps",        _wfsAmpsBuffer);
        _computeShader.SetBuffer(_kernelCSMain, "_wfsDelays",      _wfsDelaysBuffer);
        _computeShader.SetBuffer(_kernelCSMain, "_wfsSpeakerMask", _wfsSpeakerMaskBuffer);
    }

    /// <summary>Dispatch the compute shader — pipeline-agnostic.</summary>
    private void DispatchCompute()
    {
        _computeShader.SetTexture(_kernelCSMain, "Result", _target);

        int groupsX = Mathf.CeilToInt(_target.width  / 8.0f);
        int groupsY = Mathf.CeilToInt(_target.height / 8.0f);
        _computeShader.Dispatch(_kernelCSMain, groupsX, groupsY, 1);
    }

    private void ReleaseBuffers()
    {
        _wavePositionsBuffer?.Release();  _wavePositionsBuffer  = null;
        _wfsAmpsBuffer?.Release();        _wfsAmpsBuffer        = null;
        _wfsDelaysBuffer?.Release();      _wfsDelaysBuffer      = null;
        _wfsSpeakerMaskBuffer?.Release(); _wfsSpeakerMaskBuffer = null;
    }

    private void ReleaseTexture()
    {
        if (_target != null) { _target.Release(); _target = null; }
    }

    /// <summary>
    /// Returns the first At_Player in the scene with is3D == true, or null if none found.
    /// Used to ensure the wavefront shader always tracks a 3D (WFS-capable) source.
    /// </summary>
    private static At_Player Find3DPlayer()
    {
        foreach (At_Player p in FindObjectsOfType<At_Player>())
            if (p.is3D) return p;
        return null;
    }
}
