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

        if (master == null || player == null)
        {
            Debug.LogWarning("[SoundWaveShaderManager] At_MasterOutput or 3D At_Player not found — Init() aborted.");
            return;
        }

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
        // Resolve references if not set in the Inspector
        if (master == null) master = FindObjectOfType<At_MasterOutput>();
        if (player == null) player = Find3DPlayer();

        if (master == null || player == null)
        {
            // Engine not ready yet — Start() will retry, or the user must call Init() manually.
            Debug.LogWarning("[SoundWaveShaderManager] Auto-init deferred: " +
                             "At_MasterOutput or 3D At_Player not found in scene.");
            return;
        }

        Init();

        // Sync the secondary source size that was set before this object existed
        if (_initialized)
            SetSecondarySourceSize(master.secondarySourceSize);
    }

    private void Update()
    {
        if (!_initialized) return;

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
