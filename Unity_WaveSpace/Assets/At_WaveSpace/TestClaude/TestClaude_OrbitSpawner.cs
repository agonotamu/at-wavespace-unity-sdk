/// @file TestClaude_OrbitSpawner.cs
/// @brief Scène de test de charge : N copies de la même source qui tournent autour de l'auditeur.
///
/// @details
/// Crée les At_Player à l'exécution (chemin « joueur dynamique » d'At_Player.OnEnable),
/// sans passer par le fichier d'état de la scène. Le nombre de sources se règle en cours
/// de lecture (boutons à l'écran, flèches haut/bas, ou champ « Source Count » de l'Inspector)
/// pour chercher à l'oreille le nombre à partir duquel l'audio décroche.
///
/// Prérequis dans la scène : un At_MasterOutput initialisé, un At_Listener et le rig de
/// haut-parleurs virtuels (la scène TestClaude_Orbit les reprend de Assets/Scenes/debug.unity).

using System.Collections.Generic;
using System.IO;
using UnityEngine;

public class TestClaude_OrbitSpawner : MonoBehaviour
{
    [Header("Sources")]
    [Tooltip("Nombre de sources voulu. Modifiable pendant la lecture.")]
    [Range(0, 200)] public int sourceCount = 4;

    [Tooltip("Fichier audio, relatif à StreamingAssets.")]
    public string audioFile = "Audio/Speech_sample.wav";

    [Tooltip("Gain de chaque source (dB), avant compensation.")]
    public float gainDb = 0f;

    [Tooltip("Retire 10·log10(N) dB à chaque source pour garder un niveau global à peu près constant.")]
    public bool compensateGain = true;

    [Tooltip("Délai entre deux créations (s). Décale aussi les voix entre elles.")]
    public float spawnInterval = 0.35f;

    [Header("Orbite autour de l'auditeur")]
    [Tooltip("Rayon de l'orbite (m). Le rig de la scène debug est un cercle de 5 m de rayon et l'auditeur est à 2,5 m de son centre : à 9 m, les sources restent toujours à l'extérieur du rig.")]
    public float radius = 9f;

    [Tooltip("Hauteur des sources par rapport à l'auditeur (m).")]
    public float height = 0f;

    [Tooltip("Vitesse angulaire (°/s). 0 = sources immobiles.")]
    public float degreesPerSecond = 20f;

    [Tooltip("Affiche une petite sphère à la position de chaque source.")]
    public bool showMarkers = true;

    // Angle d'or : l'emplacement d'une source ne dépend pas du nombre total, donc
    // ajouter ou retirer une source ne déplace pas les autres.
    private const float GOLDEN_ANGLE_DEG = 137.50776f;

    private readonly List<At_Player> m_players = new List<At_Player>();
    private readonly List<int> m_slots = new List<int>();
    private At_MasterOutput m_master;
    private At_Listener m_listener;
    private float m_nextSpawnTime;
    private float m_orbitAngleDeg;
    private int m_createdCount;
    private float m_smoothedDeltaTime = 0.016f;

    private void Start()
    {
        m_master   = FindObjectOfType<At_MasterOutput>();
        m_listener = FindObjectOfType<At_Listener>();

        if (m_master == null)
            Debug.LogError("[TestClaude] Aucun At_MasterOutput dans la scène : aucune source ne sera créée.");
    }

    private void Update()
    {
        m_smoothedDeltaTime = Mathf.Lerp(m_smoothedDeltaTime, Time.unscaledDeltaTime, 0.05f);

        if (m_master == null || !m_master.isInitialized) return;

        // Une création à la fois, espacées de spawnInterval ; les retraits sont immédiats.
        if (m_players.Count < sourceCount && Time.time >= m_nextSpawnTime)
        {
            SpawnOne();
            m_nextSpawnTime = Time.time + spawnInterval;
        }
        while (m_players.Count > sourceCount)
            RemoveLast();

        m_orbitAngleDeg = Mathf.Repeat(m_orbitAngleDeg + degreesPerSecond * Time.deltaTime, 360f);

        float gain = CurrentGainDb();
        for (int i = 0; i < m_players.Count; i++)
        {
            m_players[i].transform.position = OrbitPosition(m_slots[i]);
            m_players[i].gain = gain;
        }
    }

    private float CurrentGainDb()
    {
        int n = Mathf.Max(1, m_players.Count);
        return gainDb - (compensateGain ? 10f * Mathf.Log10(n) : 0f);
    }

    private Vector3 OrbitPosition(int slot)
    {
        Vector3 center = m_listener != null ? m_listener.transform.position : Vector3.zero;
        float a = (m_orbitAngleDeg + slot * GOLDEN_ANGLE_DEG) * Mathf.Deg2Rad;
        return center + new Vector3(radius * Mathf.Sin(a), height, radius * Mathf.Cos(a));
    }

    private void SpawnOne()
    {
        int slot = m_createdCount++;

        // GameObject inactif : Awake()/OnEnable() d'At_Player ne s'exécutent qu'à l'activation,
        // une fois les champs renseignés (addPlayer() lit is3D et isLooping).
        var go = new GameObject($"TestClaude_Source_{slot:00}");
        go.SetActive(false);
        go.transform.SetParent(transform, false);
        go.transform.position = OrbitPosition(slot);

        var p = go.AddComponent<At_Player>();
        p.guid              = System.Guid.NewGuid().ToString();
        p.fileName          = Path.Combine(Application.streamingAssetsPath, audioFile);
        p.is3D              = true;
        p.isLooping         = true;
        p.isPlayingOnAwake  = true;
        p.playbackSpeed     = 1f;
        p.attenuation       = 0f;      // pas d'atténuation de distance : niveau indépendant du rayon
        p.minDistance       = 0.1f;
        p.gain              = CurrentGainDb();
        p.isDynamicInstance = true;

        if (showMarkers)
        {
            GameObject marker = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            Destroy(marker.GetComponent<Collider>());
            marker.name = "marker";
            marker.transform.SetParent(go.transform, false);
            marker.transform.localScale = Vector3.one * 0.3f;
        }

        go.SetActive(true);

        if (!p.isInitialized)
        {
            Debug.LogError($"[TestClaude] La source {slot} n'a pas pu être enregistrée dans le moteur.");
            Destroy(go);
            return;
        }

        m_players.Add(p);
        m_slots.Add(slot);
    }

    private void RemoveLast()
    {
        int last = m_players.Count - 1;
        // Destroy() déclenche At_Player.OnDisable(), qui arrête et retire le joueur du moteur.
        if (m_players[last] != null)
            Destroy(m_players[last].gameObject);
        m_players.RemoveAt(last);
        m_slots.RemoveAt(last);
    }

    private void OnGUI()
    {
        Event e = Event.current;
        if (e.type == EventType.KeyDown)
        {
            if (e.keyCode == KeyCode.UpArrow   || e.keyCode == KeyCode.KeypadPlus)  { sourceCount = Mathf.Min(200, sourceCount + 1); e.Use(); }
            if (e.keyCode == KeyCode.DownArrow || e.keyCode == KeyCode.KeypadMinus) { sourceCount = Mathf.Max(0,  sourceCount - 1); e.Use(); }
        }

        GUILayout.BeginArea(new Rect(10, 10, 380, 190), GUI.skin.box);
        GUILayout.Label($"TestClaude — sources en orbite : {m_players.Count} actives / {sourceCount} demandées");

        GUILayout.BeginHorizontal();
        if (GUILayout.Button("-4")) sourceCount = Mathf.Max(0,  sourceCount - 4);
        if (GUILayout.Button("-1")) sourceCount = Mathf.Max(0,  sourceCount - 1);
        if (GUILayout.Button("+1")) sourceCount = Mathf.Min(200, sourceCount + 1);
        if (GUILayout.Button("+4")) sourceCount = Mathf.Min(200, sourceCount + 4);
        GUILayout.EndHorizontal();

        GUILayout.Label($"Gain par source : {CurrentGainDb():F1} dB    Orbite : {radius:F1} m, {degreesPerSecond:F0} °/s");

        if (m_master != null)
        {
            // Pas de référence à binauralRenderMode : ce script doit aussi compiler avec
            // l'ancienne version du moteur (Unity_WaveSpace_oldVersion), qui ne l'a pas.
            string mode = m_master.isBinauralVirtualization ? "downmix stéréo" : "sorties directes";
            GUILayout.Label($"Moteur : {(m_master.isInitialized ? "démarré" : "ARRÊTÉ")} — {m_master.numVirtualSpeakers} HP virtuels, {mode}");
            GUILayout.Label($"Périphérique : {m_master.audioDeviceName} — buffer demandé {m_master.bufferSize}");
        }

        GUILayout.Label($"Affichage : {1f / Mathf.Max(1e-4f, m_smoothedDeltaTime):F0} images/s    (flèches haut/bas : ±1 source)");
        GUILayout.EndArea();
    }
}
