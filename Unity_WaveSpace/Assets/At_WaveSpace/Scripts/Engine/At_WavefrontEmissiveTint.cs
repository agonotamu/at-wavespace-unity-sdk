using UnityEngine;

/// <summary>
/// Tints this object's emission with the WFS wavefront signal from a SoundWaveShaderManager,
/// without reading back the GPU texture or modifying the object's shader/material asset.
///
/// Samples the wavefront at several points spread over the object's world-space bounds
/// (projected onto XZ, ignoring Y) and averages them, giving a more representative colour
/// than a single point sample — useful for large flat surfaces like walls.
///
/// Requires the object's material to use a shader with an _EmissionColor property and
/// the _EMISSION keyword enabled (true for Unity's default/URP Lit material: tick
/// "Emission" in the Inspector once, so the keyword is baked into the material — this
/// script only changes the colour value every frame via a MaterialPropertyBlock, so the
/// material asset itself (and its shader) is never modified or duplicated).
/// </summary>
[RequireComponent(typeof(Renderer))]
public class At_WavefrontEmissiveTint : MonoBehaviour
{
    [Header("Source")]
    public SoundWaveShaderManager wavefront;

    [Header("Sampling")]
    [Tooltip("Number of sample points along each axis of the object's bounds (e.g. 3 = 3x3 = 9 samples).")]
    [Range(1, 5)]
    public int samplesPerAxis = 3;

    [Header("Color Mapping")]
    [Tooltip("Color used when the sampled wavefront value is positive (matches the shader's blue channel).")]
    public Color positiveColor = Color.blue;

    [Tooltip("Color used when the sampled wavefront value is negative (matches the shader's red channel).")]
    public Color negativeColor = Color.red;

    [Tooltip("Multiplies the sampled magnitude before it's used as emission intensity. " +
             "Raise this if the wavefront values are small and the emission looks too dim.")]
    public float emissionIntensity = 1f;

    private Renderer _renderer;
    private MaterialPropertyBlock _propertyBlock;
    private Vector3[] _localSamplePoints;
    private static readonly int EmissionColorId = Shader.PropertyToID("_EmissionColor");

    private void Awake()
    {
        _renderer = GetComponent<Renderer>();
        _propertyBlock = new MaterialPropertyBlock();
    }

    private void OnEnable()
    {
        BuildSamplePoints();
    }

    private void OnValidate()
    {
        // Rebuild if samplesPerAxis is tweaked in the Inspector during play mode.
        if (_renderer != null)
            BuildSamplePoints();
    }

    /// <summary>
    /// Precomputes sample positions in local space, spread evenly over the renderer's
    /// local bounds on X and Z (Y is ignored — the wavefront is an XZ-plane phenomenon).
    /// </summary>
    private void BuildSamplePoints()
    {
        Bounds localBounds = GetComponent<Renderer>().localBounds;
        int n = Mathf.Max(1, samplesPerAxis);
        _localSamplePoints = new Vector3[n * n];

        for (int ix = 0; ix < n; ix++)
        {
            // Centred sample positions: e.g. for n=3, t = -1, 0, 1 (normalised to bounds extents).
            float tx = n == 1 ? 0f : Mathf.Lerp(-1f, 1f, ix / (float)(n - 1));

            for (int iz = 0; iz < n; iz++)
            {
                float tz = n == 1 ? 0f : Mathf.Lerp(-1f, 1f, iz / (float)(n - 1));

                _localSamplePoints[ix * n + iz] = localBounds.center + new Vector3(
                    tx * localBounds.extents.x,
                    0f,
                    tz * localBounds.extents.z
                );
            }
        }
    }

    private void Update()
    {
        if (wavefront == null || _localSamplePoints == null) return;

        float sum = 0f;
        for (int i = 0; i < _localSamplePoints.Length; i++)
        {
            Vector3 worldPoint = transform.TransformPoint(_localSamplePoints[i]);
            sum += wavefront.EvaluateWaveAt(worldPoint);
        }
        float average = sum / _localSamplePoints.Length;

        Color tint = average >= 0f ? positiveColor : negativeColor;
        float intensity = Mathf.Abs(average) * emissionIntensity;

        _renderer.GetPropertyBlock(_propertyBlock);
        _propertyBlock.SetColor(EmissionColorId, tint * intensity);
        _renderer.SetPropertyBlock(_propertyBlock);
    }
}
