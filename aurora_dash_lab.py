import json
import subprocess
import threading
import time
from collections import deque
from datetime import datetime

import dash
from dash import dcc, html, dash_table
from dash.dependencies import Input, Output, State
import plotly.graph_objects as go
import plotly.express as px


# -----------------------------
# Config
# -----------------------------
import sys
import os

# Determina il comando corretto per aurora_x in base al sistema operativo
if sys.platform == "win32":
    AURORA_CMD = ["aurora_x.exe", "--interactive-lab"]
else:
    AURORA_CMD = ["./aurora_x", "--interactive-lab"]

CONFIG_PATH = "aurora_interactive_config.json"

# Crea il file di config con valori di default se non esiste
if not os.path.exists(CONFIG_PATH):
    default_config = {
        "alpha_up": 0.10,
        "alpha_down": 0.02,
        "panic_boost_steps": 3,
        "success_prob_nerve": 0.95,
        "success_prob_gland": 0.95,
        "success_prob_muscle": 0.95,
    }
    with open(CONFIG_PATH, "w") as f:
        json.dump(default_config, f, indent=2)
    print(f"[INFO] Creato file di config: {CONFIG_PATH}")

# Buffer in memoria per i dati (aumentato per più storia)
health_buf = {
    "NERVE": deque(maxlen=1000),
    "GLAND": deque(maxlen=1000),
    "MUSCLE": deque(maxlen=1000),
}

# Statistiche aggregate
stats = {
    "total_events": 0,
    "last_update": None,
    "start_time": datetime.now(),
}

proc = None


def start_aurora():
    global proc
    # Verifica che l'eseguibile esista
    exe_name = AURORA_CMD[0]
    if not os.path.exists(exe_name):
        print(f"\n[ERRORE] File non trovato: {exe_name}")
        print(f"Assicurati che {exe_name} sia nella stessa directory di questo script.")
        print(f"Directory corrente: {os.getcwd()}\n")
        sys.exit(1)
    
    print(f"[INFO] Avvio di {exe_name}...")
    print(f"[INFO] Comando: {' '.join(AURORA_CMD)}")
    proc = subprocess.Popen(
        AURORA_CMD,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,  # Anche stderr per vedere eventuali errori
        stdin=subprocess.PIPE,
        text=True,
        bufsize=1,  # Line buffered
    )
    print(f"[INFO] {exe_name} avviato (PID: {proc.pid})")
    print(f"[INFO] Attesa output da {exe_name}...")


def reader_thread():
    """Legge stdout di aurora_x e accumula gli eventi health."""
    if proc is None or proc.stdout is None:
        print("[ERROR] Processo o stdout non disponibile")
        return
    
    print("[INFO] Reader thread avviato, in attesa di eventi...")
    line_count = 0
    json_count = 0
    last_event_time = time.time()
    
    try:
        for line in proc.stdout:
            line_count += 1
            original_line = line
            line = line.strip()
            
            # Debug: mostra le prime righe per capire cosa arriva
            if line_count <= 20:
                print(f"[DEBUG] Linea {line_count}: {repr(line[:150])}")
            
            # Ignora righe vuote
            if not line:
                continue
            
            # Cerca JSON anche se non inizia con {
            json_start = line.find("{")
            if json_start >= 0:
                line = line[json_start:]  # Estrai solo la parte JSON
            elif not line.startswith("{"):
                # Se non c'è JSON, salta (ma logga le prime)
                if line_count <= 20:
                    print(f"[DEBUG] Linea non-JSON saltata: {line[:80]}")
                continue
            
            try:
                evt = json.loads(line)
                json_count += 1
                last_event_time = time.time()
                
                # Debug: mostra i primi eventi JSON
                if json_count <= 10:
                    print(f"[DEBUG] ✅ Evento JSON {json_count}: type={evt.get('type')}, class={evt.get('class')}")
                
                if evt.get("type") == "health":
                    cls = evt.get("class", "")
                    if cls in health_buf:
                        health_buf[cls].append(evt)
                        stats["total_events"] += 1
                        stats["last_update"] = datetime.now()
                        if stats["total_events"] <= 20:
                            print(f"[INFO] ✅ Evento health ricevuto: {cls} - step={evt.get('step')}, cov={evt.get('cov', 0):.3f}")
                    else:
                        print(f"[WARN] Classe sconosciuta: {cls}")
                else:
                    if json_count <= 10:
                        print(f"[DEBUG] Evento non-health: type={evt.get('type')}")
                        
            except json.JSONDecodeError as e:
                if line_count <= 30:
                    print(f"[WARN] ❌ JSON decode error: {e}")
                    print(f"[WARN]    Linea problematica: {repr(line[:100])}")
                continue
            except Exception as e:
                print(f"[ERROR] Errore nel reader: {e}")
                import traceback
                traceback.print_exc()
                continue
            
            # Timeout check: se non arrivano eventi da 30 secondi, avvisa
            if time.time() - last_event_time > 30 and stats["total_events"] == 0:
                print(f"[WARN] ⚠️  Nessun evento ricevuto da 30 secondi. Verifica che aurora_x stia emettendo JSON.")
                last_event_time = time.time()  # Reset per non spammare
                
    except Exception as e:
        print(f"[ERROR] Reader thread crashato: {e}")
        import traceback
        traceback.print_exc()


# Avvia aurora_x e il thread lettore
start_aurora()
t = threading.Thread(target=reader_thread, daemon=True)
t.start()

# Thread per leggere stderr (per vedere eventuali errori)
def stderr_reader():
    if proc and proc.stderr:
        for line in proc.stderr:
            line = line.strip()
            if line:
                print(f"[STDERR] {line}")

stderr_thread = threading.Thread(target=stderr_reader, daemon=True)
stderr_thread.start()

# -----------------------------
# Dashboard Dash Avanzata
# -----------------------------

app = dash.Dash(__name__)
app.title = "Aurora Advanced Lab Dashboard"

# Aggiungi CSS personalizzato tramite index_string
app.index_string = '''
<!DOCTYPE html>
<html>
    <head>
        {%metas%}
        <title>{%title%}</title>
        {%favicon%}
        {%css%}
        <style>
            @keyframes pulse {
                0%, 100% { opacity: 1; }
                50% { opacity: 0.5; }
            }
            .slider-container {
                margin-bottom: 20px;
            }
            .slider-label {
                display: block;
                margin-bottom: 8px;
                font-weight: 500;
            }
        </style>
    </head>
    <body>
        {%app_entry%}
        <footer>
            {%config%}
            {%scripts%}
            {%renderer%}
        </footer>
    </body>
</html>
'''

# Tema dark professionale
DARK_THEME = {
    "bg": "#0d1117",
    "card_bg": "#161b22",
    "border": "#30363d",
    "text": "#c9d1d9",
    "text_muted": "#8b949e",
    "primary": "#58a6ff",
    "success": "#3fb950",
    "warning": "#d29922",
    "danger": "#f85149",
}


def write_config(alpha_up, alpha_down, panic_steps, p_nerve, p_gland, p_muscle):
    cfg = {
        "alpha_up": alpha_up,
        "alpha_down": alpha_down,
        "panic_boost_steps": int(panic_steps),
        "success_prob_nerve": p_nerve,
        "success_prob_gland": p_gland,
        "success_prob_muscle": p_muscle,
    }
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


def create_kpi_card(title, value, color=DARK_THEME["primary"], subtitle=""):
    """Crea una card KPI con stile moderno"""
    return html.Div(
        style={
            "backgroundColor": DARK_THEME["card_bg"],
            "border": f"1px solid {DARK_THEME['border']}",
            "borderRadius": "8px",
            "padding": "20px",
            "textAlign": "center",
            "boxShadow": "0 2px 4px rgba(0,0,0,0.3)",
        },
        children=[
            html.Div(
                style={"fontSize": "14px", "color": DARK_THEME["text_muted"], "marginBottom": "8px"},
                children=title
            ),
            html.Div(
                style={"fontSize": "32px", "fontWeight": "bold", "color": color, "marginBottom": "4px"},
                children=value
            ),
            html.Div(
                style={"fontSize": "12px", "color": DARK_THEME["text_muted"]},
                children=subtitle
            ) if subtitle else None,
        ]
    )


app.layout = html.Div(
    style={
        "backgroundColor": DARK_THEME["bg"],
        "color": DARK_THEME["text"],
        "fontFamily": "'Segoe UI', system-ui, sans-serif",
        "padding": "20px",
        "minHeight": "100vh",
    },
    children=[
        # Header con titolo prominente
        html.Div(
            style={
                "backgroundColor": DARK_THEME["card_bg"],
                "border": f"1px solid {DARK_THEME['border']}",
                "borderRadius": "8px",
                "padding": "25px",
                "marginBottom": "30px",
                "boxShadow": "0 4px 6px rgba(0,0,0,0.3)",
            },
            children=[
                html.Div(
                    style={
                        "display": "flex",
                        "justifyContent": "space-between",
                        "alignItems": "center",
                    },
                    children=[
                        html.Div(
                            children=[
                                html.H1(
                                    "🔬 Aurora Advanced Lab Dashboard",
                                    style={
                                        "margin": "0 0 10px 0",
                                        "color": DARK_THEME["primary"],
                                        "fontSize": "36px",
                                        "fontWeight": "bold",
                                        "textShadow": "2px 2px 4px rgba(0,0,0,0.5)",
                                    },
                                ),
                                html.P(
                                    "Monitoraggio in tempo reale del sistema Aurora con controllo parametri interattivo",
                                    style={
                                        "margin": "0",
                                        "color": DARK_THEME["text_muted"],
                                        "fontSize": "16px",
                                    },
                                ),
                            ],
                        ),
                        html.Div(
                            id="status-indicator",
                            style={
                                "display": "flex",
                                "flexDirection": "column",
                                "alignItems": "center",
                                "gap": "8px",
                                "padding": "15px",
                                "backgroundColor": DARK_THEME["bg"],
                                "borderRadius": "8px",
                                "border": f"1px solid {DARK_THEME['border']}",
                            },
                            children=[
                                html.Div(
                                    id="status-led",
                                    style={
                                        "width": "16px",
                                        "height": "16px",
                                        "borderRadius": "50%",
                                        "backgroundColor": DARK_THEME["success"],
                                        "boxShadow": f"0 0 12px {DARK_THEME['success']}",
                                        "animation": "pulse 2s infinite",
                                    }
                                ),
                                html.Span("LIVE", style={"fontSize": "14px", "fontWeight": "bold", "color": DARK_THEME["success"]}),
                            ]
                        ),
                    ],
                ),
            ],
        ),

        # KPI Cards Row
        html.Div(
            id="kpi-cards",
            style={
                "display": "grid",
                "gridTemplateColumns": "repeat(auto-fit, minmax(200px, 1fr))",
                "gap": "20px",
                "marginBottom": "30px",
            },
        ),

        # Main Content Grid
        html.Div(
            style={
                "display": "grid",
                "gridTemplateColumns": "320px 1fr",
                "gap": "20px",
                "minHeight": "600px",
            },
            children=[
                # Sidebar Left - Controls
                html.Div(
                    style={
                        "backgroundColor": DARK_THEME["card_bg"],
                        "border": f"1px solid {DARK_THEME['border']}",
                        "borderRadius": "8px",
                        "padding": "20px",
                        "height": "fit-content",
                        "position": "sticky",
                        "top": "20px",
                        "maxHeight": "90vh",
                        "overflowY": "auto",
                    },
                    children=[
                        html.H2(
                            "⚙️ Controlli Parametri",
                            style={
                                "marginTop": "0",
                                "marginBottom": "20px",
                                "color": DARK_THEME["primary"],
                                "fontSize": "22px",
                                "borderBottom": f"2px solid {DARK_THEME['border']}",
                                "paddingBottom": "10px",
                            },
                        ),
                        
                        html.H4(
                            "🧬 Parametri Organismo",
                            style={
                                "fontSize": "18px",
                                "marginTop": "20px",
                                "marginBottom": "15px",
                                "color": DARK_THEME["text"],
                                "fontWeight": "bold",
                            },
                        ),
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "α_up (reazione immunitaria)",
                                    style={
                                        "fontSize": "13px",
                                        "color": DARK_THEME["text"],
                                        "fontWeight": "500",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="alpha-up",
                                    min=0.0, max=0.5, step=0.01, value=0.10,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                    marks={0.0: "0", 0.25: "0.25", 0.5: "0.5"},
                                ),
                                html.Div(
                                    id="alpha-up-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": DARK_THEME["primary"],
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "α_down (dimagrimento)",
                                    style={
                                        "fontSize": "13px",
                                        "color": DARK_THEME["text"],
                                        "fontWeight": "500",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="alpha-down",
                                    min=0.0, max=0.1, step=0.005, value=0.02,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                    marks={0.0: "0", 0.05: "0.05", 0.1: "0.1"},
                                ),
                                html.Div(
                                    id="alpha-down-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": DARK_THEME["primary"],
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "Panic Boost Steps",
                                    style={
                                        "fontSize": "13px",
                                        "color": DARK_THEME["text"],
                                        "fontWeight": "500",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="panic-steps",
                                    min=0, max=10, step=1, value=3,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                    marks={0: "0", 5: "5", 10: "10"},
                                ),
                                html.Div(
                                    id="panic-steps-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": DARK_THEME["primary"],
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Hr(style={"borderColor": DARK_THEME["border"], "margin": "20px 0"}),
                        
                        html.H4(
                            "📡 Probabilità Canale",
                            style={
                                "fontSize": "18px",
                                "marginBottom": "15px",
                                "color": DARK_THEME["text"],
                                "fontWeight": "bold",
                            },
                        ),
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "NERVE",
                                    style={
                                        "fontSize": "13px",
                                        "color": "#1f77b4",
                                        "fontWeight": "bold",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="p-nerve",
                                    min=0.5, max=1.0, step=0.01, value=0.95,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                ),
                                html.Div(
                                    id="p-nerve-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": "#1f77b4",
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "GLAND",
                                    style={
                                        "fontSize": "13px",
                                        "color": "#ff7f0e",
                                        "fontWeight": "bold",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="p-gland",
                                    min=0.5, max=1.0, step=0.01, value=0.95,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                ),
                                html.Div(
                                    id="p-gland-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": "#ff7f0e",
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Div(
                            className="slider-container",
                            children=[
                                html.Label(
                                    "MUSCLE",
                                    style={
                                        "fontSize": "13px",
                                        "color": "#2ca02c",
                                        "fontWeight": "bold",
                                        "marginBottom": "8px",
                                        "display": "block",
                                    }
                                ),
                                dcc.Slider(
                                    id="p-muscle",
                                    min=0.5, max=1.0, step=0.01, value=0.95,
                                    tooltip={"placement": "bottom", "always_visible": True},
                                ),
                                html.Div(
                                    id="p-muscle-value",
                                    style={
                                        "fontSize": "12px",
                                        "color": "#2ca02c",
                                        "marginTop": "5px",
                                        "marginBottom": "20px",
                                        "fontWeight": "bold",
                                    }
                                ),
                            ],
                        ),
                        
                        html.Hr(style={"borderColor": DARK_THEME["border"], "margin": "20px 0"}),
                        
                        html.Div(id="config-status", style={
                            "fontSize": "11px",
                            "color": DARK_THEME["success"],
                            "padding": "10px",
                            "backgroundColor": DARK_THEME["bg"],
                            "borderRadius": "4px",
                            "border": f"1px solid {DARK_THEME['border']}",
                        }),
                        
                        html.Button(
                            "🔄 Reset Stats",
                            id="reset-btn",
                            n_clicks=0,
                            style={
                                "width": "100%",
                                "marginTop": "15px",
                                "padding": "10px",
                                "backgroundColor": DARK_THEME["warning"],
                                "color": "white",
                                "border": "none",
                                "borderRadius": "4px",
                                "cursor": "pointer",
                                "fontWeight": "bold",
                            },
                        ),
                    ],
                ),

                # Main Content Area
                html.Div(
                    children=[
                        # Charts Row 1
                        html.Div(
                            style={"display": "grid", "gridTemplateColumns": "1fr 1fr", "gap": "20px", "marginBottom": "20px"},
                            children=[
                                dcc.Graph(id="graph-cov", style={"height": "350px"}),
                                dcc.Graph(id="graph-fail", style={"height": "350px"}),
                            ],
                        ),
                        
                        # Charts Row 2
                        html.Div(
                            style={"display": "grid", "gridTemplateColumns": "1fr 1fr", "gap": "20px", "marginBottom": "20px"},
                            children=[
                                dcc.Graph(id="graph-streaks", style={"height": "300px"}),
                                dcc.Graph(id="graph-state-timeline", style={"height": "300px"}),
                            ],
                        ),
                        
                        # Stats Table
                        html.Div(
                            style={
                                "backgroundColor": DARK_THEME["card_bg"],
                                "border": f"1px solid {DARK_THEME['border']}",
                                "borderRadius": "8px",
                                "padding": "20px",
                            },
                            children=[
                                html.H3("📊 Statistiche Dettagliate", style={"marginTop": "0"}),
                                html.Div(id="stats-table"),
                            ],
                        ),
                    ],
                ),
            ],
        ),

        dcc.Interval(id="tick", interval=500, n_intervals=0),
        dcc.Store(id="reset-store", data=0),
    ],
)


# Callbacks per valori slider
@app.callback(
    [Output("alpha-up-value", "children"),
     Output("alpha-down-value", "children"),
     Output("panic-steps-value", "children"),
     Output("p-nerve-value", "children"),
     Output("p-gland-value", "children"),
     Output("p-muscle-value", "children")],
    [Input("alpha-up", "value"),
     Input("alpha-down", "value"),
     Input("panic-steps", "value"),
     Input("p-nerve", "value"),
     Input("p-gland", "value"),
     Input("p-muscle", "value")],
)
def update_slider_values(au, ad, ps, pn, pg, pm):
    return (
        f"Valore: {au:.3f}",
        f"Valore: {ad:.3f}",
        f"Valore: {int(ps)}",
        f"Valore: {pn:.2f}",
        f"Valore: {pg:.2f}",
        f"Valore: {pm:.2f}",
    )


@app.callback(
    Output("config-status", "children"),
    [Input("alpha-up", "value"),
     Input("alpha-down", "value"),
     Input("panic-steps", "value"),
     Input("p-nerve", "value"),
     Input("p-gland", "value"),
     Input("p-muscle", "value")],
)
def update_config(alpha_up, alpha_down, panic_steps, p_nerve, p_gland, p_muscle):
    write_config(alpha_up, alpha_down, panic_steps, p_nerve, p_gland, p_muscle)
    return f"✅ Config salvata: {datetime.now().strftime('%H:%M:%S')}"


@app.callback(
    [Output("kpi-cards", "children"),
     Output("graph-cov", "figure"),
     Output("graph-fail", "figure"),
     Output("graph-streaks", "figure"),
     Output("graph-state-timeline", "figure"),
     Output("stats-table", "children")],
    [Input("tick", "n_intervals"),
     Input("reset-btn", "n_clicks")],
)
def update_all(n_intervals, reset_clicks):
    # Calcola statistiche aggregate
    all_data = []
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        all_data.extend(list(health_buf[cls]))
    
    if not all_data:
        empty_fig = go.Figure()
        empty_fig.update_layout(
            paper_bgcolor=DARK_THEME["card_bg"],
            plot_bgcolor=DARK_THEME["bg"],
            font_color=DARK_THEME["text"],
            xaxis=dict(showgrid=False),
            yaxis=dict(showgrid=False),
        )
        return (
            [create_kpi_card("Eventi", "0", DARK_THEME["text_muted"])],
            empty_fig, empty_fig, empty_fig, empty_fig,
            html.Div("Nessun dato disponibile", style={"color": DARK_THEME["text_muted"]}),
        )
    
    # KPI Cards
    latest_nerve = list(health_buf["NERVE"])[-1] if health_buf["NERVE"] else None
    latest_gland = list(health_buf["GLAND"])[-1] if health_buf["GLAND"] else None
    latest_muscle = list(health_buf["MUSCLE"])[-1] if health_buf["MUSCLE"] else None
    
    avg_cov = sum(d.get("cov", 0) for d in all_data) / len(all_data) if all_data else 0
    avg_fail = sum(d.get("fail", 0) for d in all_data) / len(all_data) if all_data else 0
    
    kpi_cards = [
        create_kpi_card("Eventi Totali", f"{stats['total_events']}", DARK_THEME["primary"]),
        create_kpi_card("Coverage Media", f"{avg_cov:.3f}", DARK_THEME["success"] if avg_cov > 0.9 else DARK_THEME["warning"]),
        create_kpi_card("Fail Rate Media", f"{avg_fail:.3f}", DARK_THEME["danger"] if avg_fail > 0.1 else DARK_THEME["success"]),
        create_kpi_card("Uptime", f"{(datetime.now() - stats['start_time']).seconds // 60}m", DARK_THEME["text_muted"]),
    ]
    
    # Grafici
    colors = {
        "NERVE": "#1f77b4",
        "GLAND": "#ff7f0e",
        "MUSCLE": "#2ca02c",
    }
    
    # Coverage Graph
    fig_cov = go.Figure()
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        data = list(health_buf[cls])
        if data:
            x = [d["step"] for d in data]
            cov = [d.get("cov", 0.0) for d in data]
            fig_cov.add_trace(go.Scatter(
                x=x, y=cov, mode="lines+markers", name=f"{cls}",
                line={"color": colors[cls], "width": 2},
                marker={"size": 3},
                hovertemplate=f"<b>{cls}</b><br>Step: %{{x}}<br>Coverage: %{{y:.4f}}<extra></extra>",
            ))
    fig_cov.update_layout(
        title={"text": "📈 EWMA Coverage per Classe", "font": {"size": 18}},
        xaxis_title="Step",
        yaxis_title="Coverage",
        paper_bgcolor=DARK_THEME["card_bg"],
        plot_bgcolor=DARK_THEME["bg"],
        font_color=DARK_THEME["text"],
        hovermode="x unified",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="right", x=1),
    )
    
    # Fail Rate Graph
    fig_fail = go.Figure()
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        data = list(health_buf[cls])
        if data:
            x = [d["step"] for d in data]
            fail = [d.get("fail", 0.0) for d in data]
            fig_fail.add_trace(go.Scatter(
                x=x, y=fail, mode="lines+markers", name=f"{cls}",
                line={"color": colors[cls], "width": 2},
                marker={"size": 3},
                fill="tozeroy",
                fillcolor=colors[cls],
                opacity=0.3,
                hovertemplate=f"<b>{cls}</b><br>Step: %{{x}}<br>Fail Rate: %{{y:.4f}}<extra></extra>",
            ))
    fig_fail.update_layout(
        title={"text": "⚠️ EWMA Fail Rate per Classe", "font": {"size": 18}},
        xaxis_title="Step",
        yaxis_title="Fail Rate",
        paper_bgcolor=DARK_THEME["card_bg"],
        plot_bgcolor=DARK_THEME["bg"],
        font_color=DARK_THEME["text"],
        hovermode="x unified",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="right", x=1),
    )
    
    # Streaks Graph
    fig_streaks = go.Figure()
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        data = list(health_buf[cls])
        if data:
            x = [d["step"] for d in data]
            gs = [d.get("gs", 0) for d in data]
            bs = [d.get("bs", 0) for d in data]
            fig_streaks.add_trace(go.Scatter(
                x=x, y=gs, mode="lines", name=f"{cls} Good",
                line={"color": colors[cls], "width": 2, "dash": "solid"},
                stackgroup="good",
            ))
            fig_streaks.add_trace(go.Scatter(
                x=x, y=[-b for b in bs], mode="lines", name=f"{cls} Bad",
                line={"color": colors[cls], "width": 2, "dash": "dot"},
                stackgroup="bad",
            ))
    fig_streaks.update_layout(
        title={"text": "📊 Good/Bad Streaks", "font": {"size": 18}},
        xaxis_title="Step",
        yaxis_title="Streak",
        paper_bgcolor=DARK_THEME["card_bg"],
        plot_bgcolor=DARK_THEME["bg"],
        font_color=DARK_THEME["text"],
        hovermode="x unified",
    )
    
    # State Timeline
    fig_timeline = go.Figure()
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        data = list(health_buf[cls])
        if data:
            x = [d["step"] for d in data]
            safety_states = [d.get("safety", "UNKNOWN") for d in data]
            mode_states = [d.get("mode", "UNKNOWN") for d in data]
            
            # Converti safety states a numeri per visualizzazione
            safety_map = {"HEALTHY": 3, "DEGRADED": 2, "CRITICAL": 1, "UNKNOWN": 0}
            safety_nums = [safety_map.get(s, 0) for s in safety_states]
            
            fig_timeline.add_trace(go.Scatter(
                x=x, y=safety_nums, mode="lines+markers", name=f"{cls} Safety",
                line={"color": colors[cls], "width": 2},
                marker={"size": 4},
                hovertemplate=f"<b>{cls}</b><br>Step: %{{x}}<br>Safety: %{{text}}<br>Mode: %{{customdata}}<extra></extra>",
                text=safety_states,
                customdata=mode_states,
            ))
    fig_timeline.update_layout(
        title={"text": "🔄 Safety State Timeline", "font": {"size": 18}},
        xaxis_title="Step",
        yaxis_title="Safety Level",
        paper_bgcolor=DARK_THEME["card_bg"],
        plot_bgcolor=DARK_THEME["bg"],
        font_color=DARK_THEME["text"],
        yaxis=dict(tickmode="array", tickvals=[1, 2, 3], ticktext=["CRITICAL", "DEGRADED", "HEALTHY"]),
        hovermode="x unified",
    )
    
    # Stats Table
    table_data = []
    for cls in ["NERVE", "GLAND", "MUSCLE"]:
        data = list(health_buf[cls])
        if data:
            latest = data[-1]
            table_data.append({
                "Classe": cls,
                "Coverage": f"{latest.get('cov', 0):.4f}",
                "Fail Rate": f"{latest.get('fail', 0):.4f}",
                "Good Streak": latest.get('gs', 0),
                "Bad Streak": latest.get('bs', 0),
                "Safety": latest.get('safety', 'N/A'),
                "Mode": latest.get('mode', 'N/A'),
            })
    
    stats_table = dash_table.DataTable(
        data=table_data,
        columns=[{"name": col, "id": col} for col in ["Classe", "Coverage", "Fail Rate", "Good Streak", "Bad Streak", "Safety", "Mode"]],
        style_cell={
            "backgroundColor": DARK_THEME["bg"],
            "color": DARK_THEME["text"],
            "textAlign": "left",
            "padding": "10px",
            "fontFamily": "monospace",
        },
        style_header={
            "backgroundColor": DARK_THEME["card_bg"],
            "color": DARK_THEME["primary"],
            "fontWeight": "bold",
            "border": f"1px solid {DARK_THEME['border']}",
        },
        style_data={
            "border": f"1px solid {DARK_THEME['border']}",
        },
        style_data_conditional=[
            {
                "if": {"filter_query": "{Classe} = NERVE"},
                "color": colors["NERVE"],
                "fontWeight": "bold",
            },
            {
                "if": {"filter_query": "{Classe} = GLAND"},
                "color": colors["GLAND"],
                "fontWeight": "bold",
            },
            {
                "if": {"filter_query": "{Classe} = MUSCLE"},
                "color": colors["MUSCLE"],
                "fontWeight": "bold",
            },
        ],
    )
    
    return kpi_cards, fig_cov, fig_fail, fig_streaks, fig_timeline, stats_table


if __name__ == "__main__":
    time.sleep(1.0)
    print("\n" + "="*70)
    print("🚀 Aurora Advanced Lab Dashboard")
    print("="*70)
    print(f"\n📊 Dashboard disponibile su: http://127.0.0.1:8050")
    print(f"   Oppure: http://localhost:8050")
    print("\n💡 Funzionalità:")
    print("   • Grafici interattivi in tempo reale")
    print("   • KPI cards con statistiche aggregate")
    print("   • Timeline degli stati safety/mode")
    print("   • Tabelle dettagliate per classe")
    print("   • Controlli parametri in tempo reale")
    print("\n⚠️  Premi Ctrl+C per fermare la dashboard\n")
    print("="*70 + "\n")
    app.run(debug=False, host='127.0.0.1', port=8050)
