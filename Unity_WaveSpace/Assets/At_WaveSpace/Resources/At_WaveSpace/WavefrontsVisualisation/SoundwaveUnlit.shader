Shader "AT_WaveSpace/SoundwaveUnlit"
{
    Properties
    {
        _MainTex  ("Render Texture", 2D) = "black" {}
        _Intensity ("Intensity",  Float) = 1.0
    }

    // ─────────────────────────────────────────────────────────────────────────
    // SubShader 1 : URP
    // PackageRequirements évite la compilation (et l'erreur Core.hlsl introuvable)
    // sur les projets sans URP installé. Requiert Unity 2021.2+.
    // ─────────────────────────────────────────────────────────────────────────
    SubShader
    {
        PackageRequirements { "com.unity.render-pipelines.universal" }

        Tags
        {
            "RenderType"      = "Transparent"
            "Queue"           = "Transparent"
            "RenderPipeline"  = "UniversalPipeline"
            "IgnoreProjector" = "True"
        }

        Pass
        {
            Name "SoundwaveUnlit_URP"
            // SRPDefaultUnlit : URP skips all light contribution on this pass
            Tags { "LightMode" = "SRPDefaultUnlit" }

            ZWrite Off
            Blend SrcAlpha OneMinusSrcAlpha
            Cull Off

            HLSLPROGRAM
            #pragma vertex   vert
            #pragma fragment frag
            #pragma target   3.5

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            struct Attributes
            {
                float4 positionOS : POSITION;
                float2 uv         : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            TEXTURE2D(_MainTex);
            SAMPLER(sampler_MainTex);

            CBUFFER_START(UnityPerMaterial)
                float4 _MainTex_ST;
                float  _Intensity;
            CBUFFER_END

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                UNITY_SETUP_INSTANCE_ID(IN);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(OUT);
                OUT.positionHCS = TransformObjectToHClip(IN.positionOS.xyz);
                OUT.uv          = TRANSFORM_TEX(IN.uv, _MainTex);
                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                half4 col = SAMPLE_TEXTURE2D(_MainTex, sampler_MainTex, IN.uv);
                col.rgb  *= _Intensity;
                return col;
            }
            ENDHLSL
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // SubShader 2 : Built-in RP  (fallback for HDRP too)
    // ─────────────────────────────────────────────────────────────────────────
    SubShader
    {
        Tags
        {
            "RenderType"      = "Transparent"
            "Queue"           = "Transparent"
            "IgnoreProjector" = "True"
        }

        Pass
        {
            Name "SoundwaveUnlit_Builtin"

            ZWrite Off
            Blend SrcAlpha OneMinusSrcAlpha
            Cull Off

            CGPROGRAM
            #pragma vertex   vert
            #pragma fragment frag
            #pragma target   3.5

            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4    _MainTex_ST;
            float     _Intensity;

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv     : TEXCOORD0;
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv  : TEXCOORD0;
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv  = TRANSFORM_TEX(v.uv, _MainTex);
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                fixed4 col = tex2D(_MainTex, i.uv);
                col.rgb   *= _Intensity;
                return col;
            }
            ENDCG
        }
    }

    FallBack "Hidden/InternalErrorShader"
}
