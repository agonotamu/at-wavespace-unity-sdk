/// @file TestClaude_StaticOrbit.cs
/// @brief Scène de test de charge avec des joueurs statiques : fait tourner autour de
///        l'auditeur tous les At_Player enfants de cet objet.
///
/// @details
/// Les joueurs existent dans la scène (100 par défaut, voir TestClaude_StaticSceneBuilder),
/// GameObject désactivé. On les active un par un pendant la lecture : dans la Hierarchy,
/// avec les boutons à l'écran, ou avec les flèches haut/bas. Ce script ne crée rien :
/// il place les joueurs sur l'orbite et ajuste leur gain selon le nombre de joueurs actifs.
///
/// Compile avec la version actuelle du moteur et avec Unity_WaveSpace_oldVersion.

using UnityEngine;

public class TestClaude_StaticOrbit : MonoBehaviour
{
    [Header("Niveau")]
    [Tooltip("Gain de chaque source (dB), avant compensation.")]
    public float gainDb = 0f;

    [Tooltip("Retire 10·log10(N actifs) dB à chaque source pour garder un niveau global à peu près constant.")]
    public bool compensateGain = true;

    [Header("Orbite autour de l'auditeur")]
    [Tooltip("Rayon de l'orbite (m). À 9 m, les sources restent à l'extérieur du rig de la scène.")]
    public float radius = 9f;

    [Tooltip("Hauteur des sources par rapport à l'auditeur (m).")]
    public float height = 0f;

    [Tooltip("Vitesse angulaire (°/s). 0 = sources immobiles.")]
    public float degreesPerSecond = 20f;

    // Angle d'or : l'emplacement d'un joueur ne dépend que de son rang, pas du nombre de joueurs actifs.
    private const float GOLDEN_ANGLE_DEG = 137.50776f;

    private At_Player[] m_players = new At_Player[0];
    private At_Listener m_listener;
    private float m_orbitAngleDeg;
    private int m_activeCount;
    private float m_smoothedDeltaTime = 0.016f;

    private void Awake()
    {
        m_listener = FindObjectOfType<At_Listener>();
        m_players  = GetComponentsInChildren<At_Player>(true);   // joueurs désactivés compris, dans l'ordre de la Hierarchy
        PlaceAll();   // avant toute activation : un joueur démarre déjà à sa place
    }

    private void Update()
    {
        m_smoothedDeltaTime = Mathf.Lerp(m_smoothedDeltaTime, Time.unscaledDeltaTime, 0.05f);
        m_orbitAngleDeg = Mathf.Repeat(m_orbitAngleDeg + degreesPerSecond * Time.deltaTime, 360f);

        m_activeCount = 0;
        foreach (At_Player p in m_players)
            if (p != null && p.gameObject.activeInHierarchy) m_activeCount++;

        PlaceAll();
    }

    private float CurrentGainDb()
    {
        return gainDb - (compensateGain ? 10f * Mathf.Log10(Mathf.Max(1, m_activeCount)) : 0f);
    }

    private void PlaceAll()
    {
        Vector3 center = m_listener != null ? m_listener.transform.position : Vector3.zero;
        float gain = CurrentGainDb();

        for (int i = 0; i < m_players.Length; i++)
        {
            if (m_players[i] == null) continue;
            float a = (m_orbitAngleDeg + i * GOLDEN_ANGLE_DEG) * Mathf.Deg2Rad;
            m_players[i].transform.position = center + new Vector3(radius * Mathf.Sin(a), height, radius * Mathf.Cos(a));
            m_players[i].gain = gain;
        }
    }

    /// <summary>Active (delta > 0) les premiers joueurs inactifs, ou désactive (delta < 0) les derniers actifs.</summary>
    private void ChangeActiveCount(int delta)
    {
        if (delta > 0)
        {
            for (int i = 0; i < m_players.Length && delta > 0; i++)
                if (m_players[i] != null && !m_players[i].gameObject.activeSelf) { m_players[i].gameObject.SetActive(true); delta--; }
        }
        else
        {
            for (int i = m_players.Length - 1; i >= 0 && delta < 0; i--)
                if (m_players[i] != null && m_players[i].gameObject.activeSelf) { m_players[i].gameObject.SetActive(false); delta++; }
        }
    }

    private void OnGUI()
    {
        Event e = Event.current;
        if (e.type == EventType.KeyDown)
        {
            if (e.keyCode == KeyCode.UpArrow   || e.keyCode == KeyCode.KeypadPlus)  { ChangeActiveCount(+1); e.Use(); }
            if (e.keyCode == KeyCode.DownArrow || e.keyCode == KeyCode.KeypadMinus) { ChangeActiveCount(-1); e.Use(); }
        }

        GUILayout.BeginArea(new Rect(10, 10, 380, 150), GUI.skin.box);
        GUILayout.Label($"TestClaude — joueurs statiques en orbite : {m_activeCount} actifs / {m_players.Length}");

        GUILayout.BeginHorizontal();
        if (GUILayout.Button("-4")) ChangeActiveCount(-4);
        if (GUILayout.Button("-1")) ChangeActiveCount(-1);
        if (GUILayout.Button("+1")) ChangeActiveCount(+1);
        if (GUILayout.Button("+4")) ChangeActiveCount(+4);
        GUILayout.EndHorizontal();

        GUILayout.Label($"Gain par source : {CurrentGainDb():F1} dB    Orbite : {radius:F1} m, {degreesPerSecond:F0} °/s");
        GUILayout.Label($"Affichage : {1f / Mathf.Max(1e-4f, m_smoothedDeltaTime):F0} images/s    (flèches haut/bas : ±1 joueur)");
        GUILayout.EndArea();
    }
}
