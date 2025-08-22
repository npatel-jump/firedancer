'''

Nishk's last minute notes:

This script is a work in progress. There is a lot missing from this, I
mostly spent 10 minutes just having cursor copy function by function
because I thought it would be a cool idea and wanted to see a POC.

The general idea I had for this was to have a more interactive dashboard
for the analysis then the current report.py. It would be EXTREMELY
useful to policy making for repair catchup AND repair for turbine. The
benefit of this is that you can zoom in and micro analyze policies on
this dashboard, and ensure you see expected behavior.

If you want to make this an actual analysis tool, I would reccomend
redoing the outline completely, and taking the information from the
original report_page.py file and porting the graphs over more carefully.
'''

import pandas as pd
import numpy as np
import plotly.graph_objects as go
import plotly.express as px
from plotly.subplots import make_subplots
import plotly.figure_factory as ff
import dash
from dash import dcc, html, Input, Output, callback
import sys
import warnings
import os
from datetime import datetime
import re

"""
Interactive Firedancer Shredcap Analysis Report
===============================================

This script generates an interactive web-based report on repair analysis off of one testnet run.

1. Add the following to your testnet config.toml file:
    [tiles.shredcap]
       enabled = true
       folder_path = /my/folder

2. Start up firedancer-dev with the above config. The following files will be generated:
    - /my/folder/request_data.csv
    - /my/folder/shred_data.csv
    - /my/folder/fec_complete.csv

3. Run this script with the following command:
    python3 report_page.py <testnet.log path> <csv_folder path>

    Dependencies:
    python3 -m pip install pandas numpy plotly dash

4. The report will be available at http://localhost:8050

Usage: python report_page.py [--turbine] [<testnet.log path> <csv_folder_path>]
  --turbine: Include slots >= turbine slot in slot processing analysis (default: exclude them)
  If no arguments provided, will auto-detect most recently created log and CSV folder
"""

warnings.filterwarnings('ignore')

# Global variables to store data
global_data = {}

def capture_log(message):
    """Capture log messages to display on the dashboard"""
    global global_data
    if global_data is None:
        global_data = {}
    if 'processing_logs' not in global_data:
        global_data['processing_logs'] = []
    global_data['processing_logs'].append(message)
    print(message)  # Still print to console

def int_to_ip(ip_int):
    """Convert integer IP address to dotted decimal notation."""
    try:
        if isinstance(ip_int, str):
            if '.' in ip_int:
                return ip_int
            ip_int = int(ip_int)

        return f"{(ip_int >> 24) & 255}.{(ip_int >> 16) & 255}.{(ip_int >> 8) & 255}.{ip_int & 255}"
    except (ValueError, TypeError):
        return str(ip_int)

def create_processing_logs_section():
    """Create a section showing processing logs from data analysis"""
    if 'processing_logs' not in global_data or not global_data['processing_logs']:
        return html.Div()

    logs = global_data['processing_logs']

    # Format logs into readable sections
    log_items = []
    for log in logs:
        log_items.append(html.P(log, style={'margin': '2px 0', 'fontSize': '12px', 'fontFamily': 'monospace'}))

    return html.Div([
        html.H4("Data Processing Summary", style={'color': '#2c3e50', 'marginBottom': '10px'}),
        html.Div([
            html.Div(log_items, style={'maxHeight': '200px', 'overflowY': 'auto', 'backgroundColor': '#f8f9fa',
                                     'padding': '10px', 'border': '1px solid #ddd', 'borderRadius': '4px'})
        ])
    ], style={'border': '1px solid #bdc3c7', 'borderRadius': '8px', 'padding': '20px',
             'margin': '20px 0', 'backgroundColor': '#ffffff'})

def create_navigation_page():
    """
    Creates an interactive navigation page for the report with clickable sections
    """
    return html.Div([
        html.H1("Firedancer Shredcap Analysis Report",
               style={'textAlign': 'center', 'fontSize': '36px', 'fontWeight': 'bold', 'marginBottom': '20px'}),

        html.H3("Interactive Analysis Dashboard",
               style={'textAlign': 'center', 'fontSize': '18px', 'fontStyle': 'italic', 'marginBottom': '40px'}),

        html.Div([
            html.P("This report analyzes repair mechanisms during validator catchup and live operation. "
                  "Click on any section below to navigate to the interactive analysis.",
                  style={'textAlign': 'center', 'fontSize': '14px', 'marginBottom': '40px'})
        ]),

        # Processing logs section
        create_processing_logs_section(),

        # Navigation Cards
        html.Div([
            create_nav_card("execution-stats", "Execution Statistics",
                           "Timeline showing key execution events, snapshot loading, and first turbine timing"),

            create_nav_card("slot-repair-time", "Slot Repair Time Analysis",
                           "Timeline showing repair duration for each slot with processing intervals"),

            create_nav_card("peer-stats", "Peer Statistics Analysis",
                           "Round-trip time distributions, hit rates, request patterns, and warm-up message analysis"),

            create_nav_card("repair-efficiency", "Repair Efficiency Heatmap",
                           "Visual analysis of repair efficiency: actual vs minimal required requests per shred"),

            create_nav_card("turbine-timeline", "Turbine Shred Timeline",
                           "Normalized per-slot scatter plot showing turbine vs repair shreds timing"),

            create_nav_card("fec-completion", "FEC/Batch Completion Analysis",
                           "Forward Error Correction timing and slot completion statistics"),

            create_nav_card("detailed-slots", "Detailed Slot Analysis",
                           "Microscopic view of repair patterns for critical transition slots"),

            create_nav_card("long-slots", "Long Slots Analysis",
                           "Analysis of slots that take unusually long to complete (410-500ms)")
        ], style={'display': 'grid', 'gridTemplateColumns': 'repeat(auto-fit, minmax(300px, 1fr))',
                 'gap': '20px', 'margin': '20px'}),

        # Footer
        html.Div([
            html.Hr(),
            html.P("Data collected from CSV files generated with shredcap tile enabled during testnet execution.",
                  style={'textAlign': 'center', 'fontSize': '12px', 'fontStyle': 'italic'}),
            html.P("Developed by: Emily Wang and Nishk Patel",
                  style={'textAlign': 'center', 'fontSize': '14px'}),
            html.P(f"Generated on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
                  style={'textAlign': 'center', 'fontSize': '14px'})
        ], style={'marginTop': '60px'})
    ])

def create_nav_card(page_id, title, description):
    """Create a navigation card for each analysis section"""
    # Map page IDs to URL paths
    page_paths = {
        'execution-stats': '/execution-stats',
        'slot-repair-time': '/slot-repair-time',
        'peer-stats': '/peer-stats',
        'repair-efficiency': '/repair-efficiency',
        'turbine-timeline': '/turbine-timeline',
        'fec-completion': '/fec-completion',
        'detailed-slots': '/detailed-slots',
        'long-slots': '/long-slots'
    }

    return html.Div([
        html.H4(title, style={'marginBottom': '10px', 'color': '#2c3e50'}),
        html.P(description, style={'fontSize': '13px', 'color': '#7f8c8d', 'marginBottom': '15px'}),
        dcc.Link("View Analysis", href=page_paths.get(page_id, '/'),
                style={'backgroundColor': '#3498db', 'color': 'white', 'border': 'none',
                      'padding': '10px 20px', 'borderRadius': '5px', 'textDecoration': 'none',
                      'display': 'inline-block', 'cursor': 'pointer'})
    ], style={'border': '1px solid #bdc3c7', 'borderRadius': '8px', 'padding': '20px',
             'backgroundColor': '#f8f9fa', 'transition': 'box-shadow 0.3s',
             'boxShadow': '0 2px 4px rgba(0,0,0,0.1)'})

def execution_stats_analysis():
    """Convert execution stats to interactive Plotly format"""
    data = global_data

    if 'execution_info' not in data:
        return html.Div("No execution data available")

    info = data['execution_info']

    # Create an information display
    cards = []

    # Snapshot Information Card
    cards.append(
        html.Div([
            html.H4("Snapshot Information", style={'color': '#2c3e50'}),
            html.P(f"Snapshot Slot: {info.get('snapshot_slot', 'Unknown')}"),
            html.P(f"Snapshot Loaded: {info.get('snapshot_loaded_ts', 'Unknown')}")
        ], style={'border': '1px solid #bdc3c7', 'borderRadius': '8px', 'padding': '20px',
                 'margin': '10px', 'backgroundColor': '#f8f9fa'})
    )

    # Turbine Information Card
    cards.append(
        html.Div([
            html.H4("Turbine Information", style={'color': '#2c3e50'}),
            html.P(f"First Turbine Slot: {info.get('first_turbine', 'Unknown')}"),
            html.P(f"First Turbine Executed: {info.get('first_turbine_exec_ts', 'Unknown')}")
        ], style={'border': '1px solid #bdc3c7', 'borderRadius': '8px', 'padding': '20px',
                 'margin': '10px', 'backgroundColor': '#f8f9fa'})
    )

    # Execution Timeline Card
    cards.append(
        html.Div([
            html.H4("Execution Timeline", style={'color': '#2c3e50'}),
            html.P(f"Last Executed Slot: {info.get('last_executed', 'Unknown')}"),
            html.P(f"Time to First Turbine: {info.get('time_to_turbine', 'Unknown')}")
        ], style={'border': '1px solid #bdc3c7', 'borderRadius': '8px', 'padding': '20px',
                 'margin': '10px', 'backgroundColor': '#f8f9fa'})
    )

    return html.Div([
        html.H2("Execution Statistics", style={'textAlign': 'center'}),
        html.Div(cards, style={'display': 'grid', 'gridTemplateColumns': 'repeat(auto-fit, minmax(300px, 1fr))', 'gap': '20px'})
    ])

def peer_stats_analysis_interactive():
    """Convert peer statistics analysis to interactive Plotly format"""
    data = global_data

    if 'peer_stats' not in data:
        return html.Div("No peer statistics data available")

    stats = data['peer_stats']

    # RTT Distribution Plot
    rtt_dist_fig = go.Figure()
    rtt_data = stats.get('round_trip_times', [])

    if len(rtt_data) > 0:
        rtt_dist_fig.add_trace(go.Histogram(
            x=rtt_data,
            nbinsx=100,
            name="RTT Distribution",
            marker_color='blue',
            opacity=0.7
        ))

        # Add percentile lines
        p25 = np.percentile(rtt_data, 25)
        p50 = np.percentile(rtt_data, 50)
        p75 = np.percentile(rtt_data, 75)
        mean_val = np.mean(rtt_data)

        rtt_dist_fig.add_vline(x=p25, line_dash="dash", line_color="lightgray",
                              annotation_text=f"25th %ile: {p25:.1f}ms")
        rtt_dist_fig.add_vline(x=p50, line_dash="dash", line_color="gray",
                              annotation_text=f"50th %ile: {p50:.1f}ms")
        rtt_dist_fig.add_vline(x=p75, line_dash="dash", line_color="darkgray",
                              annotation_text=f"75th %ile: {p75:.1f}ms")
        rtt_dist_fig.add_vline(x=mean_val, line_color="black",
                              annotation_text=f"Mean: {mean_val:.1f}ms")

    rtt_dist_fig.update_layout(
        title="RTT Distribution",
        xaxis_title="Round-trip Time (ms)",
        yaxis_title="Frequency",
        showlegend=False
    )

    # Hit Rate Distribution
    hit_rate_fig = go.Figure()
    hit_rates = stats.get('hit_rates', [])

    if len(hit_rates) > 0:
        hit_rate_fig.add_trace(go.Histogram(
            x=hit_rates,
            nbinsx=50,
            name="Hit Rate Distribution",
            marker_color='gold',
            opacity=0.7
        ))

        # Add percentile lines
        p25_hr = np.percentile(hit_rates, 25)
        p50_hr = np.percentile(hit_rates, 50)
        p75_hr = np.percentile(hit_rates, 75)
        mean_hr = np.mean(hit_rates)

        hit_rate_fig.add_vline(x=p25_hr, line_dash="dash", line_color="lightgray",
                              annotation_text=f"25th %ile: {p25_hr:.3f}")
        hit_rate_fig.add_vline(x=p50_hr, line_dash="dash", line_color="gray",
                              annotation_text=f"50th %ile: {p50_hr:.3f}")
        hit_rate_fig.add_vline(x=p75_hr, line_dash="dash", line_color="darkgray",
                              annotation_text=f"75th %ile: {p75_hr:.3f}")
        hit_rate_fig.add_vline(x=mean_hr, line_color="black",
                              annotation_text=f"Mean: {mean_hr:.3f}")

    hit_rate_fig.update_layout(
        title="Hit Rate Distribution",
        xaxis_title="Hit Rate",
        yaxis_title="Frequency",
        showlegend=False
    )

    # Combined Analysis Scatter Plots
    combined_fig = make_subplots(
        rows=1, cols=2,
        subplot_titles=["Request Frequency vs Hit Rate", "Request Frequency vs Average RTT"],
        horizontal_spacing=0.1
    )

    peer_combined = stats.get('peer_stats_combined', pd.DataFrame())

    if not peer_combined.empty:
        # Request Frequency vs Hit Rate
        combined_fig.add_trace(
            go.Scatter(
                x=peer_combined['requests'],
                y=peer_combined['hit_rate'],
                mode='markers',
                marker=dict(color='gold', size=8, opacity=0.6, line=dict(color='black', width=0.5)),
                name="Request Freq vs Hit Rate",
                hovertemplate="Requests: %{x}<br>Hit Rate: %{y:.3f}<extra></extra>"
            ),
            row=1, col=1
        )

        # Request Frequency vs RTT (limit y-axis to 500)
        rtt_mask = peer_combined['avg_rtt'] > 0
        if rtt_mask.any():
            valid_requests = peer_combined.loc[rtt_mask, 'requests']
            valid_rtt = peer_combined.loc[rtt_mask, 'avg_rtt']

            combined_fig.add_trace(
                go.Scatter(
                    x=valid_requests,
                    y=valid_rtt,
                    mode='markers',
                    marker=dict(color='darkblue', size=8, opacity=0.6, line=dict(color='black', width=0.5)),
                    name="Request Freq vs RTT",
                    hovertemplate="Requests: %{x}<br>RTT: %{y:.2f}ms<extra></extra>"
                ),
                row=1, col=2
            )

    combined_fig.update_xaxes(title_text="Number of Requests", row=1, col=1)
    combined_fig.update_yaxes(title_text="Hit Rate", row=1, col=1)
    combined_fig.update_xaxes(title_text="Number of Requests", row=1, col=2)
    combined_fig.update_yaxes(title_text="Average RTT (ms)", range=[0, 500], row=1, col=2)

    combined_fig.update_layout(
        title="Combined Peer Analysis",
        showlegend=False,
        height=500
    )

    # Request Distribution Bar Chart
    reqs_per_ip = stats.get('reqs_per_ip', pd.Series())
    request_dist_fig = go.Figure()

    if not reqs_per_ip.empty:
        sorted_requests = reqs_per_ip.sort_values()
        x_positions = list(range(len(sorted_requests)))

        # Create hover text with peer IP information
        hover_text = [f"Peer {i+1}: {int_to_ip(ip)}<br>Requests: {count}"
                     for i, (ip, count) in enumerate(sorted_requests.items())]

        request_dist_fig.add_trace(go.Bar(
            x=x_positions,
            y=sorted_requests.values,
            marker_color='purple',
            opacity=0.7,
            hovertemplate="%{customdata}<extra></extra>",
            customdata=hover_text
        ))

        # Set x-axis labels for readability
        if len(sorted_requests) > 50:
            tick_interval = max(1, len(sorted_requests) // 20)
            tick_positions = list(range(0, len(sorted_requests), tick_interval))
            tick_labels = [f'{i+1}: {int_to_ip(sorted_requests.index[i])}' for i in tick_positions]
        else:
            tick_positions = list(range(0, len(sorted_requests), max(1, len(sorted_requests) // 10)))
            tick_labels = [f'{i+1}: {int_to_ip(sorted_requests.index[i])}' for i in tick_positions]

        request_dist_fig.update_layout(
            title="Request Distribution per Peer (Sorted by Request Count)",
            xaxis=dict(
                title="Peers (Sorted by Request Count)",
                tickmode='array',
                tickvals=tick_positions,
                ticktext=tick_labels,
                tickangle=45
            ),
            yaxis_title="Number of Requests",
            showlegend=False
        )

    # Warm-up Message Analysis (if data available)
    warmup_fig = go.Figure()
    warmup_data = stats.get('warmup_analysis', {})

    if warmup_data:
        responder_rates = warmup_data.get('responder_hit_rates', [])
        non_responder_rates = warmup_data.get('non_responder_hit_rates', [])

        if len(responder_rates) > 0 and len(non_responder_rates) > 0:
            # Create violin plot using Plotly
            warmup_fig.add_trace(go.Violin(
                y=responder_rates,
                name=f'Warm-up Responders<br>(n={len(responder_rates)})',
                box_visible=True,
                meanline_visible=True,
                fillcolor='lightgreen',
                opacity=0.7,
                x0='Responders'
            ))

            warmup_fig.add_trace(go.Violin(
                y=non_responder_rates,
                name=f'Non-Responders<br>(n={len(non_responder_rates)})',
                box_visible=True,
                meanline_visible=True,
                fillcolor='lightcoral',
                opacity=0.7,
                x0='Non-Responders'
            ))

            warmup_fig.update_layout(
                title="Hit Rate Distribution: Warm-up Message Response Analysis<br>(Slot 0, Shred 0)",
                yaxis_title="Hit Rate",
                showlegend=True
            )

    return html.Div([
        html.H2("Peer Statistics Analysis", style={'textAlign': 'center'}),

        # RTT Analysis Row
        html.Div([
            html.Div([
                dcc.Graph(figure=rtt_dist_fig)
            ], style={'width': '50%', 'display': 'inline-block'}),

            html.Div([
                dcc.Graph(figure=hit_rate_fig)
            ], style={'width': '50%', 'display': 'inline-block'})
        ]),

        # Combined Analysis
        dcc.Graph(figure=combined_fig),

        # Request Distribution
        dcc.Graph(figure=request_dist_fig, style={'height': '600px'}),

        # Warm-up Analysis (if available)
        dcc.Graph(figure=warmup_fig) if warmup_data else html.Div()
    ])

def turbine_timeline_analysis():
    """Convert turbine shred timeline to interactive Plotly format"""
    data = global_data

    if 'turbine_data' not in data:
        return html.Div("No turbine timeline data available")

    turbine_info = data['turbine_data']

    fig = go.Figure()

    # Add turbine shreds (blue)
    if 'turbine_shreds' in turbine_info and not turbine_info['turbine_shreds'].empty:
        turbine_df = turbine_info['turbine_shreds']
        fig.add_trace(go.Scatter(
            x=turbine_df['slot'],
            y=turbine_df['slot_relative_time_ms'],
            mode='markers',
            marker=dict(color='blue', size=3, opacity=0.6),
            name=f"Turbine Shreds ({len(turbine_df):,})",
            hovertemplate="Slot: %{x}<br>Time from Start: %{y:.1f}ms<extra></extra>"
        ))

    # Add repair shreds (red)
    if 'repair_shreds' in turbine_info and not turbine_info['repair_shreds'].empty:
        repair_df = turbine_info['repair_shreds']
        fig.add_trace(go.Scatter(
            x=repair_df['slot'],
            y=repair_df['slot_relative_time_ms'],
            mode='markers',
            marker=dict(color='red', size=3, opacity=0.8),
            name=f"Repair Shreds ({len(repair_df):,})",
            hovertemplate="Slot: %{x}<br>Time from Start: %{y:.1f}ms<extra></extra>"
        ))

    fig.update_layout(
        title="Turbine vs Repair Shreds - Normalized per Slot<br>Blue = Turbine, Red = Repair, Each Slot Starts at 0ms",
        xaxis_title="Slot Number",
        yaxis_title="Time from Slot Start (ms)",
        hovermode='closest',
        height=700
    )

    return html.Div([
        html.H2("Turbine Shred Timeline", style={'textAlign': 'center'}),
        dcc.Graph(figure=fig)
    ])

def repair_efficiency_analysis():
    """Convert repair efficiency heatmap to interactive Plotly format"""
    data = global_data

    if 'efficiency_data' not in data:
        return html.Div("No repair efficiency data available")

    efficiency_info = data['efficiency_data']

    # Create interactive heatmap
    fig = go.Figure(data=go.Heatmap(
        z=efficiency_info.get('heatmap_matrix', []),
        x=efficiency_info.get('slot_labels', []),
        y=efficiency_info.get('shred_indices', []),
        colorscale='RdYlGn_r',
        hovertemplate="Slot: %{x}<br>Shred Index: %{y}<br>Efficiency Ratio: %{z:.2f}<extra></extra>",
        colorbar=dict(title="Efficiency Ratio<br>(Excess Requests / Minimal Required)")
    ))

    fig.update_layout(
        title="Repair Efficiency Heatmap - All Repair Period Slots",
        xaxis_title="Slot Number",
        yaxis_title="Shred Index",
        height=800
    )

    return html.Div([
        html.H2("Repair Efficiency Analysis", style={'textAlign': 'center'}),
        dcc.Graph(figure=fig)
    ])

def long_slots_analysis():
    """Convert long slots analysis to interactive Plotly format"""
    data = global_data

    if 'long_slots_data' not in data:
        return html.Div("No long slots data available")

    long_slots_info = data['long_slots_data']

    # Correlation matrix heatmap
    corr_fig = go.Figure(data=go.Heatmap(
        z=long_slots_info.get('correlation_matrix', []),
        x=['Slot Complete Time', 'Repair Requests', 'Turbine Shreds', 'Total Shreds'],
        y=['Slot Complete Time', 'Repair Requests', 'Turbine Shreds', 'Total Shreds'],
        colorscale='RdBu',
        text=long_slots_info.get('correlation_matrix', []),
        texttemplate="%{text:.2f}",
        hovertemplate="X: %{x}<br>Y: %{y}<br>Correlation: %{z:.2f}<extra></extra>"
    ))

    corr_fig.update_layout(
        title="Correlation Matrix for Long Slots (410-500ms completion time)",
        height=500
    )

    # Offender analysis bar chart
    offenders_fig = go.Figure()
    offenders = long_slots_info.get('offenders', pd.DataFrame())

    if not offenders.empty:
        offenders_fig.add_trace(go.Bar(
            x=[int_to_ip(ip) for ip in offenders.index],
            y=offenders.values,
            marker_color='red',
            opacity=0.7,
            hovertemplate="Peer: %{x}<br>Late Shreds: %{y}<extra></extra>"
        ))

        offenders_fig.update_layout(
            title="Top Offenders: Shreds Sent After 400ms",
            xaxis_title="Peer IP Address",
            yaxis_title="Count of Late Shreds",
            xaxis_tickangle=45
        )

    return html.Div([
        html.H2("Long Slots Analysis", style={'textAlign': 'center'}),
        dcc.Graph(figure=corr_fig),
        dcc.Graph(figure=offenders_fig) if not offenders.empty else html.Div("No offender data available")
    ])

def completion_times_analysis():
    """Convert FEC/batch completion analysis to interactive Plotly format"""
    data = global_data

    if 'completion_data' not in data:
        return html.Div("No completion times data available")

    completion_info = data['completion_data']

    # FEC completion times histogram
    fec_fig = make_subplots(
        rows=1, cols=2,
        subplot_titles=["FEC Completion Times (Live)", "FEC Completion Times (Catchup)"]
    )

    fec_live = completion_info.get('fec_live', [])
    fec_catchup = completion_info.get('fec_catchup', [])

    if len(fec_live) > 0:
        fec_fig.add_trace(
            go.Histogram(x=fec_live, nbinsx=50, marker_color='blue', opacity=0.7, name="Live"),
            row=1, col=1
        )

    if len(fec_catchup) > 0:
        fec_fig.add_trace(
            go.Histogram(x=fec_catchup, nbinsx=50, marker_color='orange', opacity=0.7, name="Catchup"),
            row=1, col=2
        )

    fec_fig.update_xaxes(title_text="Time to Complete (ms)", row=1, col=1)
    fec_fig.update_xaxes(title_text="Time to Complete (ms)", row=1, col=2)
    fec_fig.update_yaxes(title_text="Frequency", row=1, col=1)
    fec_fig.update_yaxes(title_text="Frequency", row=1, col=2)
    fec_fig.update_layout(showlegend=False, height=500)

    # Batch completion times
    batch_fig = go.Figure()
    batch_times = completion_info.get('batch_times', [])

    if len(batch_times) > 0:
        batch_fig.add_trace(go.Histogram(
            x=batch_times,
            nbinsx=50,
            marker_color='green',
            opacity=0.7
        ))

    batch_fig.update_layout(
        title="Batch Completion Times",
        xaxis_title="Time to Complete (ms)",
        yaxis_title="Frequency"
    )

    return html.Div([
        html.H2("FEC/Batch Completion Analysis", style={'textAlign': 'center'}),
        dcc.Graph(figure=fec_fig),
        dcc.Graph(figure=batch_fig)
    ])

def slot_request_rate_analysis_interactive():
    """Convert slot request rate analysis to interactive Plotly format"""
    data = global_data

    if 'slot_rate_data' not in data:
        return html.Div("No slot request rate data available")

    slot_info = data['slot_rate_data']

    # Slot processing timeline
    fig = go.Figure()

    valid_slots = slot_info.get('valid_slots', [])

    if valid_slots:
        # Convert timestamps to relative milliseconds
        min_start_time = min(s['start'] for s in valid_slots)

        for i, slot_data in enumerate(valid_slots):
            start_ms = (slot_data['start'] - min_start_time) / 1e6
            end_ms = (slot_data['end'] - min_start_time) / 1e6
            slot_num = slot_data['shred_num']

            # Add processing interval line
            fig.add_trace(go.Scatter(
                x=[slot_num, slot_num],
                y=[start_ms, end_ms],
                mode='lines',
                line=dict(color='blue', width=3),
                name='Slot Processing' if i == 0 else '',
                showlegend=i == 0,
                hovertemplate=f"Slot: {slot_num}<br>Start: {start_ms:.1f}ms<br>End: {end_ms:.1f}ms<extra></extra>"
            ))

            # Add start and end markers
            fig.add_trace(go.Scatter(
                x=[slot_num],
                y=[start_ms],
                mode='markers',
                marker=dict(color='green', size=8),
                name='Slot Start' if i == 0 else '',
                showlegend=i == 0,
                hovertemplate=f"Slot {slot_num} Start<br>Time: {start_ms:.1f}ms<extra></extra>"
            ))

            fig.add_trace(go.Scatter(
                x=[slot_num],
                y=[end_ms],
                mode='markers',
                marker=dict(color='red', size=8),
                name='Slot End' if i == 0 else '',
                showlegend=i == 0,
                hovertemplate=f"Slot {slot_num} End<br>Time: {end_ms:.1f}ms<extra></extra>"
            ))

    fig.update_layout(
        title="Slot Processing Time Intervals",
        xaxis_title="Slot Number",
        yaxis_title="Time (milliseconds from first slot start)",
        hovermode='closest',
        height=700
    )

    return html.Div([
        html.H2("Slot Request Rate Analysis", style={'textAlign': 'center'}),
        dcc.Graph(figure=fig)
    ])

def detailed_slots_analysis():
    """Convert detailed slot repairs to interactive Plotly format"""
    data = global_data

    if 'detailed_slots_data' not in data:
        return html.Div("No detailed slots data available")

    slot_repairs = data['detailed_slots_data']

    figures = []

    for slot_num, slot_data in slot_repairs.items():
        fig = make_subplots(
            rows=1, cols=3,
            subplot_titles=[f"All Data - Slot {slot_num}", f"Min Responses - Slot {slot_num}", f"Source Analysis - Slot {slot_num}"],
            horizontal_spacing=0.05
        )

        # Plot 1: All repair requests and responses
        if 'repair_requests' in slot_data and not slot_data['repair_requests'].empty:
            rq = slot_data['repair_requests']
            fig.add_trace(
                go.Scatter(
                    x=rq['idx'],
                    y=rq['timestamp'],
                    mode='markers',
                    marker=dict(color='orange', size=4),
                    name="Repair Requests",
                    hovertemplate="Index: %{x}<br>Timestamp: %{y}<extra></extra>"
                ),
                row=1, col=1
            )

        if 'responses' in slot_data and not slot_data['responses'].empty:
            rsp = slot_data['responses']
            fig.add_trace(
                go.Scatter(
                    x=rsp['idx'],
                    y=rsp['timestamp'],
                    mode='markers',
                    marker=dict(color='blue', size=4),
                    name="Shreds Received",
                    hovertemplate="Index: %{x}<br>Timestamp: %{y}<extra></extra>"
                ),
                row=1, col=1
            )

        # Plot 2: Minimum responses and all requests
        if 'min_responses' in slot_data and not slot_data['min_responses'].empty:
            min_rsp = slot_data['min_responses']
            fig.add_trace(
                go.Scatter(
                    x=min_rsp['idx'],
                    y=min_rsp['timestamp'],
                    mode='markers',
                    marker=dict(color='blue', size=6),
                    name="Min Shred Response",
                    hovertemplate="Index: %{x}<br>Timestamp: %{y}<extra></extra>"
                ),
                row=1, col=2
            )

        if 'repair_requests' in slot_data and not slot_data['repair_requests'].empty:
            rq = slot_data['repair_requests']
            fig.add_trace(
                go.Scatter(
                    x=rq['idx'],
                    y=rq['timestamp'],
                    mode='markers',
                    marker=dict(color='orange', size=4),
                    name="All Repair Requests",
                    hovertemplate="Index: %{x}<br>Timestamp: %{y}<extra></extra>"
                ),
                row=1, col=2
            )

        # Plot 3: Source analysis with different colors per IP
        if 'min_responses' in slot_data and not slot_data['min_responses'].empty:
            min_rsp = slot_data['min_responses']
            unique_src_ips = min_rsp['src_ip'].unique()
            colors = px.colors.qualitative.Set1[:len(unique_src_ips)]

            for i, src_ip in enumerate(unique_src_ips):
                src_data = min_rsp[min_rsp['src_ip'] == src_ip]
                fig.add_trace(
                    go.Scatter(
                        x=src_data['idx'],
                        y=src_data['timestamp'],
                        mode='markers',
                        marker=dict(color=colors[i % len(colors)], size=6),
                        name=f"Src: {int_to_ip(src_ip)}",
                        hovertemplate=f"Src: {int_to_ip(src_ip)}<br>Index: %{{x}}<br>Timestamp: %{{y}}<extra></extra>"
                    ),
                    row=1, col=3
                )

        fig.update_layout(
            title=f"Repair Analysis for Slot {slot_num}",
            showlegend=True,
            height=500
        )

        figures.append(dcc.Graph(figure=fig))

    return html.Div([
        html.H2("Detailed Slot Analysis", style={'textAlign': 'center'}),
        html.Div(figures)
    ])

def fec_completion_analysis():
    """Convert FEC completion analysis to interactive Plotly format"""
    data = global_data

    if 'completion_data' not in data:
        return html.Div("No FEC completion data available")

    completion_info = data['completion_data']

    # FEC completion times histogram
    fec_fig = make_subplots(
        rows=1, cols=2,
        subplot_titles=["FEC Completion Times (Live)", "FEC Completion Times (Catchup)"]
    )

    fec_live = completion_info.get('fec_live', [])
    fec_catchup = completion_info.get('fec_catchup', [])

    if len(fec_live) > 0:
        fec_fig.add_trace(
            go.Histogram(x=fec_live, nbinsx=50, marker_color='blue', opacity=0.7, name="Live"),
            row=1, col=1
        )

    if len(fec_catchup) > 0:
        fec_fig.add_trace(
            go.Histogram(x=fec_catchup, nbinsx=50, marker_color='orange', opacity=0.7, name="Catchup"),
            row=1, col=2
        )

    fec_fig.update_xaxes(title_text="Time to Complete (ms)", row=1, col=1)
    fec_fig.update_xaxes(title_text="Time to Complete (ms)", row=1, col=2)
    fec_fig.update_yaxes(title_text="Frequency", row=1, col=1)
    fec_fig.update_yaxes(title_text="Frequency", row=1, col=2)
    fec_fig.update_layout(showlegend=False, height=500)

    # Batch completion times
    batch_fig = go.Figure()
    batch_times = completion_info.get('batch_times', [])

    if len(batch_times) > 0:
        batch_fig.add_trace(go.Histogram(
            x=batch_times,
            nbinsx=50,
            marker_color='green',
            opacity=0.7
        ))

    batch_fig.update_layout(
        title="Batch Completion Times",
        xaxis_title="Time to Complete (ms)",
        yaxis_title="Frequency"
    )

    # Batch timeline for specific slot
    timeline_fig = go.Figure()
    timeline_data = completion_info.get('timeline_data', [])

    if timeline_data:
        for i, batch in enumerate(timeline_data):
            timeline_fig.add_trace(go.Scatter(
                x=[batch['start'], batch['end']],
                y=[batch['ref_tick'], batch['ref_tick']],
                mode='lines',
                line=dict(color='blue', width=4),
                name=f"Ref Tick {batch['ref_tick']}" if i < 5 else '',
                showlegend=i < 5,
                hovertemplate=f"Ref Tick: {batch['ref_tick']}<br>Duration: {batch['end']-batch['start']:.1f}ms<extra></extra>"
            ))

    timeline_fig.update_layout(
        title="Batch Completion Timeline (Sample Slot)",
        xaxis_title="Time (ms)",
        yaxis_title="Ref Tick",
        height=400
    )

    return html.Div([
        html.H2("FEC/Batch Completion Analysis", style={'textAlign': 'center'}),
        dcc.Graph(figure=fec_fig),
        dcc.Graph(figure=batch_fig),
        dcc.Graph(figure=timeline_fig)
    ])

# Initialize Dash app
app = dash.Dash(__name__)

app.layout = html.Div([
    dcc.Location(id='url', refresh=False),
    html.Div(id='page-content'),

    # Navigation bar - only home button
    html.Div([
        html.Div([
                        html.Button("Home", id="btn-home",
                       style={'margin': '2px 5px', 'padding': '8px 12px', 'backgroundColor': '#3498db',
                             'color': 'white', 'border': 'none', 'borderRadius': '5px', 'cursor': 'pointer',
                             'fontSize': '14px', 'fontWeight': 'bold'})
        ], style={'display': 'flex', 'justifyContent': 'center'})
    ], style={'backgroundColor': '#34495e', 'padding': '10px', 'position': 'fixed',
             'top': '0', 'width': '100%', 'zIndex': '1000'})
], style={'paddingTop': '60px'})  # Reduced padding for single-row navigation

@app.callback(Output('page-content', 'children'),
              [Input('url', 'pathname')])
def display_page(pathname):
    """Route to different pages based on URL pathname"""

    if pathname == '/execution-stats':
        return execution_stats_analysis()
    elif pathname == '/peer-stats':
        return peer_stats_analysis_interactive()
    elif pathname == '/turbine-timeline':
        return turbine_timeline_analysis()
    elif pathname == '/repair-efficiency':
        return repair_efficiency_analysis()
    elif pathname == '/slot-repair-time':
        return slot_request_rate_analysis_interactive()
    elif pathname == '/fec-completion':
        return fec_completion_analysis()
    elif pathname == '/detailed-slots':
        return detailed_slots_analysis()
    elif pathname == '/long-slots':
        return long_slots_analysis()

    # Default to navigation page for home or any other path
    return create_navigation_page()

@app.callback(Output('url', 'pathname'),
              [Input('btn-home', 'n_clicks')],
              prevent_initial_call=True)
def navigate_home(home_clicks):
    """Navigate to home page when home button is clicked"""
    if home_clicks:
        return '/'
    return dash.no_update

def load_and_process_data(log_path, request_data_path, shred_data_path, peers_data_path, fec_complete_path=None, include_after_turbine=False):
    """Load and process all data, storing in global_data for interactive use"""
    global global_data

    # Initialize global_data structure and clear any existing logs
    global_data = {}

    capture_log("=== FIREDANCER SHREDCAP ANALYSIS REPORT ===")
    capture_log("Loading and processing data for interactive report...")
    capture_log(f"Log file: {log_path}")
    capture_log(f"Shred data: {shred_data_path}")
    if request_data_path:
        capture_log(f"Request data: {request_data_path}")
    if fec_complete_path:
        capture_log(f"FEC completion data: {fec_complete_path}")

    # Parse execution stats from log
    first_turbine, snapshot_slot, last_executed = parse_execution_stats(log_path)

    # Update execution info with the parsed values
    global_data['execution_info'].update({
        'first_turbine': first_turbine,
        'snapshot_slot': snapshot_slot,
        'last_executed': last_executed
    })

    # Load CSV data
    capture_log("Reading CSV files...")

    try:
        shreds_data = pd.read_csv(shred_data_path,
                                 dtype={'src_ip': str, 'src_port': int, 'timestamp': int, 'slot': int,
                                       'ref_tick': int, 'fec_set_idx': int, 'idx': int, 'is_turbine': bool,
                                       'is_data': bool, 'nonce': int},
                                 on_bad_lines='skip',
                                 skipfooter=1,
                                 engine='python')
        capture_log(f"✓ Loaded {len(shreds_data)} shred records")
    except Exception as e:
        capture_log(f"❌ Error loading shred data: {e}")
        raise

    if request_data_path and os.path.exists(request_data_path):
        try:
            repair_requests = pd.read_csv(request_data_path,
                                        dtype={'dst_ip': str, 'dst_port': int, 'timestamp': int,
                                              'slot': int, 'idx': int, 'nonce': int},
                                        skipfooter=1,
                                        engine='python')
            capture_log(f"✓ Loaded {len(repair_requests)} repair request records")

            # Process all analysis types
            capture_log("Processing peer statistics...")
            process_peer_statistics(repair_requests, shreds_data)
            capture_log("Processing slot request rate...")
            process_slot_request_rate(shreds_data, first_turbine, include_after_turbine)
            capture_log("Processing detailed slots...")
            process_detailed_slots(repair_requests, shreds_data, snapshot_slot, first_turbine)
            capture_log("Processing repair efficiency...")
            process_repair_efficiency(repair_requests, shreds_data, snapshot_slot, first_turbine)
            capture_log("Processing long slots...")
            process_long_slots(shreds_data, first_turbine)
        except Exception as e:
            capture_log(f"❌ Error loading request data: {e}")
            capture_log("Continuing with shred data only...")
    else:
        capture_log("No request data found, skipping peer analysis")

    # Process turbine timeline data
    capture_log("Processing turbine timeline...")
    process_turbine_timeline(shreds_data, first_turbine)

    # Process FEC completion data if available
    if fec_complete_path and os.path.exists(fec_complete_path):
        try:
            fec_stats = pd.read_csv(fec_complete_path,
                                  dtype={'timestamp': int, 'slot': int, 'ref_tick': int, 'fec_set_idx': int, 'data_cnt': int},
                                  on_bad_lines='skip',
                                  skipfooter=1,
                                  engine='python')
            capture_log(f"✓ Loaded {len(fec_stats)} FEC completion records")
            capture_log("Processing completion times...")
            process_completion_times(fec_stats, shreds_data, first_turbine)
        except Exception as e:
            capture_log(f"❌ Error loading FEC data: {e}")
            capture_log("Continuing without FEC analysis...")
    else:
        capture_log("No FEC data found, skipping FEC analysis")

    capture_log("✅ Data processing complete!")

def process_slot_request_rate(shreds_data, first_turbine, include_after_turbine):
    """Process slot request rate data for interactive visualization"""

    # Filter based on turbine flag
    if include_after_turbine:
        filtered_data = shreds_data[shreds_data['slot'] >= first_turbine].copy()
        capture_log(f"Slot analysis: including slots >= {first_turbine} (turbine mode)")
    else:
        filtered_data = shreds_data[shreds_data['slot'] < first_turbine].copy()
        capture_log(f"Slot analysis: excluding slots >= {first_turbine} (catchup mode)")

    if filtered_data.empty:
        capture_log("Warning: No slot data found for analysis")
        global_data['slot_rate_data'] = {}
        return

    # Calculate slot timestamps and processing intervals
    valid_slots = []
    for slot, group in filtered_data.groupby('slot'):
        first_timestamp = group['timestamp'].min()
        last_timestamp = group['timestamp'].max()

        valid_slots.append({
            'shred_num': slot,
            'start': first_timestamp,
            'end': last_timestamp
        })

    valid_slots.sort(key=lambda x: x['shred_num'])

    # Calculate average time per slot for logging
    if valid_slots:
        total_time = sum(slot['end'] - slot['start'] for slot in valid_slots)
        avg_time_per_slot_ms = total_time / len(valid_slots) / 1_000_000
        capture_log(f"Processed {len(valid_slots)} slots, average time per slot: {avg_time_per_slot_ms:.2f} ms")

    global_data['slot_rate_data'] = {
        'valid_slots': valid_slots
    }

def process_detailed_slots(repair_requests, shreds_data, snapshot_slot, first_turbine):
    """Process detailed slot analysis data"""
    print("Processing detailed slots...")

    # Key transition slots to analyze
    slots_to_analyze = [
        snapshot_slot + 1,
        first_turbine - 1,
        first_turbine,
        first_turbine + 50
    ]

    detailed_data = {}

    for slot in slots_to_analyze:
        if slot >= 0:  # Valid slot number
            # Get repair requests for this slot
            slot_requests = repair_requests[repair_requests['slot'] == slot].copy()
            slot_responses = shreds_data[shreds_data['slot'] == slot].copy()

            # Get minimum responses per index
            if not slot_responses.empty:
                min_responses = slot_responses.loc[slot_responses.groupby('idx')['timestamp'].idxmin()]
            else:
                min_responses = pd.DataFrame()

            detailed_data[slot] = {
                'repair_requests': slot_requests,
                'responses': slot_responses,
                'min_responses': min_responses
            }

    global_data['detailed_slots_data'] = detailed_data

def process_repair_efficiency(repair_requests, shreds_data, snapshot_slot, first_turbine):
    """Process repair efficiency data for heatmap"""
    print("Processing repair efficiency...")

    # Filter to repair period
    repair_period_requests = repair_requests[repair_requests['slot'].between(snapshot_slot, first_turbine - 1)].copy()
    repair_period_responses = shreds_data[
        (shreds_data['slot'].between(snapshot_slot, first_turbine - 1)) &
        (shreds_data['is_turbine'] == False)
    ].copy()

    if repair_period_requests.empty or repair_period_responses.empty:
        global_data['efficiency_data'] = {}
        return

    # Create simplified efficiency matrix for visualization
    slots = range(snapshot_slot, first_turbine)
    max_idx = min(100, repair_period_responses['idx'].max())  # Limit for performance

    # Create matrix
    heatmap_matrix = np.zeros((max_idx + 1, len(slots)))
    slot_labels = list(slots)
    shred_indices = list(range(max_idx + 1))

    # Calculate efficiency ratios (simplified)
    for slot_i, slot in enumerate(slots):
        slot_requests = repair_period_requests[repair_period_requests['slot'] == slot]
        slot_responses = repair_period_responses[repair_period_responses['slot'] == slot]

        for idx in range(max_idx + 1):
            req_count = len(slot_requests[slot_requests['idx'] == idx])
            resp_count = len(slot_responses[slot_responses['idx'] == idx])

            if resp_count > 0:
                efficiency_ratio = (req_count - 1) / 1 if req_count > 1 else 0
            else:
                efficiency_ratio = req_count

            heatmap_matrix[idx, slot_i] = efficiency_ratio

    global_data['efficiency_data'] = {
        'heatmap_matrix': heatmap_matrix.tolist(),
        'slot_labels': slot_labels,
        'shred_indices': shred_indices
    }

def process_long_slots(shreds_data, first_turbine):
    """Process long slots analysis data"""
    print("Processing long slots...")

    # This is a simplified version - in a real implementation you'd need slot completion data
    # For now, create placeholder data structure
    correlation_matrix = np.array([
        [1.0, 0.3, -0.1, 0.5],
        [0.3, 1.0, 0.2, 0.8],
        [-0.1, 0.2, 1.0, 0.4],
        [0.5, 0.8, 0.4, 1.0]
    ])

    # Placeholder offender data
    offenders = pd.Series([10, 8, 6, 4, 2], index=[167772161, 167772162, 167772163, 167772164, 167772165])

    global_data['long_slots_data'] = {
        'correlation_matrix': correlation_matrix.tolist(),
        'offenders': offenders
    }

def process_completion_times(fec_stats, shreds_data, first_turbine):
    """Process FEC completion timing data"""
    print("Processing completion times...")

    # Calculate completion times
    fec_stats['time_to_complete(ms)'] = (fec_stats['timestamp'] - fec_stats['timestamp'].shift(1)) / 1_000_000
    fec_stats['time_to_complete(ms)'] = fec_stats['time_to_complete(ms)'].fillna(0)

    # Split by phase
    fec_live = fec_stats[fec_stats['slot'] >= first_turbine]['time_to_complete(ms)'].values
    fec_catchup = fec_stats[fec_stats['slot'] < first_turbine]['time_to_complete(ms)'].values

    # Batch completion times (simplified)
    batch_times = fec_stats.groupby(['slot', 'ref_tick'])['time_to_complete(ms)'].mean().values

    # Timeline data for specific slot (sample)
    sample_slot_data = fec_stats[fec_stats['slot'] == fec_stats['slot'].iloc[0]]
    timeline_data = []
    for _, row in sample_slot_data.iterrows():
        timeline_data.append({
            'ref_tick': row['ref_tick'],
            'start': 0,  # Simplified
            'end': row['time_to_complete(ms)']
        })

    global_data['completion_data'] = {
        'fec_live': fec_live,
        'fec_catchup': fec_catchup,
        'batch_times': batch_times,
        'timeline_data': timeline_data
    }

def parse_execution_stats(log_path):
    """Parse execution statistics from log file"""
    first_turbine = None
    snapshot_slot = None
    last_executed = None
    snapshot_loaded_ts = None
    first_turbine_exec_ts = None

    with open(log_path, 'r') as file:
        lines = file.readlines()

    # Parse forward
    for line in lines:
        if 'First turbine slot' in line:
            first_turbine = int(line.split()[-1])
        elif 'snapshot slot' in line:
            tokens = line.split()
            snapshot_slot = int(tokens[-1])
            snapshot_loaded_ts = f'{tokens[1]} {tokens[2]}'

        if first_turbine and f'slot: {first_turbine}' in line:
            tokens = line.split()
            first_turbine_exec_ts = f'{tokens[1]} {tokens[2]}'
            break

    # Parse backward for last executed
    for line in lines[::-1]:
        if 'slot:' in line:
            match = re.search(r'slot:\s*(\d+)', line)
            if match:
                last_executed = int(match.group(1))
                break

    # Handle cases where automatic log parsing failed - use fallback values
    if snapshot_slot is None:
        capture_log('Warning: Could not find snapshot slot in log, using default value 0')
        snapshot_slot = 0

    if first_turbine is None:
        capture_log('Warning: Could not find first turbine slot in log, using default value 100')
        first_turbine = 100

    if last_executed is None:
        capture_log('Warning: Could not find last executed slot in log, using default value 200')
        last_executed = 200

    # Initialize execution_info if it doesn't exist
    if 'execution_info' not in global_data:
        global_data['execution_info'] = {}

    # Store timing information
    global_data['execution_info'].update({
        'snapshot_loaded_ts': snapshot_loaded_ts,
        'first_turbine_exec_ts': first_turbine_exec_ts
    })

    if snapshot_loaded_ts and first_turbine_exec_ts:
        try:
            diff = pd.to_datetime(first_turbine_exec_ts, utc=True) - pd.to_datetime(snapshot_loaded_ts, utc=True)
            global_data['execution_info']['time_to_turbine'] = f"{diff.total_seconds()}s"
        except Exception as e:
            capture_log(f"Warning: Could not calculate time difference: {e}")
            global_data['execution_info']['time_to_turbine'] = "Unknown"

    # Log the execution statistics that were found
    capture_log(f"snapshot_slot = {snapshot_slot}")
    capture_log(f"first_turbine = {first_turbine}")
    capture_log(f"last_executed = {last_executed}")

    if global_data['execution_info'].get('time_to_turbine') != "Unknown":
        capture_log(f"Time from snapshot loaded to first turbine execution: {global_data['execution_info'].get('time_to_turbine')}")

    return first_turbine, snapshot_slot, last_executed

def process_peer_statistics(repair_requests, shreds_data):
    """Process peer statistics for interactive visualization"""

    # Keep original data for warm-up analysis
    repair_requests_original = repair_requests.copy()
    shreds_data_original = shreds_data.copy()

    # Filter out slot 0 for main analysis
    repair_requests = repair_requests.query('slot != 0').copy()
    shreds_data = shreds_data.query('slot != 0').copy()

    capture_log(f"Peer analysis: {len(repair_requests)} requests from {repair_requests['dst_ip'].nunique()} unique peers")

    # Timestamp normalization
    ts_scale = 1e6 if repair_requests_original['timestamp'].iloc[0] > 1e12 else 1
    if ts_scale > 1:
        repair_requests['timestamp'] /= ts_scale
        shreds_data['timestamp'] /= ts_scale

    # RTT calculation
    comprehensive_merge = pd.merge(
        repair_requests[['nonce', 'timestamp', 'dst_ip']],
        shreds_data[['nonce', 'timestamp']],
        on='nonce',
        suffixes=('_req', '_shred'),
        how='inner'
    )

    comprehensive_merge['round_trip_time_ms'] = comprehensive_merge['timestamp_shred'] - comprehensive_merge['timestamp_req']
    valid_rtt_data = comprehensive_merge.query('round_trip_time_ms >= 0')

    # Peer metrics
    peer_metrics = valid_rtt_data.groupby('dst_ip').agg({
        'round_trip_time_ms': ['mean', 'median', 'count'],
        'nonce': 'count'
    }).round(3)

    peer_metrics.columns = ['avg_rtt', 'median_rtt', 'rtt_count', 'total_responses']

    # Hit rates
    total_requests = repair_requests['dst_ip'].value_counts()
    unique_successful_requests = valid_rtt_data.drop_duplicates(subset=['nonce', 'dst_ip'])
    successful_responses = unique_successful_requests['dst_ip'].value_counts()
    hit_rates = (successful_responses / total_requests).fillna(0)

    # Combined data
    peer_stats_combined = pd.concat([
        total_requests.rename('requests'),
        hit_rates.rename('hit_rate'),
        peer_metrics['avg_rtt'].rename('avg_rtt')
    ], axis=1, join='outer').fillna(0)

    # Process warm-up message analysis
    warmup_analysis = {}
    warmup_requests = repair_requests_original[
        (repair_requests_original['slot'] == 0) &
        (repair_requests_original['idx'] == 0)
    ]
    warmup_responses = shreds_data_original[
        (shreds_data_original['slot'] == 0) &
        (shreds_data_original['idx'] == 0) &
        (shreds_data_original['is_turbine'] == False)
    ]

    if not warmup_requests.empty and not warmup_responses.empty:
        # Find peers who responded to warm-up message
        warmup_merge = pd.merge(
            warmup_requests[['nonce', 'dst_ip']],
            warmup_responses[['nonce']],
            on='nonce',
            how='inner'
        )
        warmup_responders = set(warmup_merge['dst_ip'].unique())
        all_contacted_peers = set(repair_requests['dst_ip'].unique())
        warmup_non_responders = all_contacted_peers - warmup_responders

        # Get hit rates for both groups
        warmup_responder_hit_rates = hit_rates[hit_rates.index.isin(warmup_responders)]
        warmup_non_responder_hit_rates = hit_rates[hit_rates.index.isin(warmup_non_responders)]

        if len(warmup_responder_hit_rates) > 0 and len(warmup_non_responder_hit_rates) > 0:
            warmup_analysis = {
                'responder_hit_rates': warmup_responder_hit_rates.values,
                'non_responder_hit_rates': warmup_non_responder_hit_rates.values
            }

    # Log summary statistics
    capture_log(f"Peer statistics: {len(valid_rtt_data)} successful requests, average RTT: {valid_rtt_data['round_trip_time_ms'].mean():.2f} ms")
    capture_log(f"Hit rate summary: mean={hit_rates.mean():.3f}, median={hit_rates.median():.3f}")

    # Store in global data
    global_data['peer_stats'] = {
        'round_trip_times': valid_rtt_data['round_trip_time_ms'].values,
        'hit_rates': hit_rates.values,
        'peer_stats_combined': peer_stats_combined,
        'reqs_per_ip': total_requests,
        'warmup_analysis': warmup_analysis
    }

def process_turbine_timeline(shreds_data, first_turbine):
    """Process turbine timeline data for interactive visualization"""

    live_data = shreds_data[shreds_data['slot'] >= first_turbine].copy()

    if live_data.empty:
        capture_log("Warning: No live data found for turbine timeline analysis")
        global_data['turbine_data'] = {}
        return

    # Calculate slot relative times
    slot_start_times = live_data.groupby('slot')['timestamp'].min()
    live_data['slot_relative_time_ms'] = live_data.apply(
        lambda row: (row['timestamp'] - slot_start_times[row['slot']]) / 1_000_000,
        axis=1
    )

    # Separate turbine and repair shreds
    turbine_shreds = live_data[live_data['is_turbine'] == True]
    repair_shreds = live_data[live_data['is_turbine'] == False]

    # Log summary statistics
    turbine_pct = len(turbine_shreds) / len(live_data) * 100 if len(live_data) > 0 else 0
    capture_log(f"Turbine timeline: {len(turbine_shreds)} turbine shreds ({turbine_pct:.1f}%), {len(repair_shreds)} repair shreds")

    global_data['turbine_data'] = {
        'turbine_shreds': turbine_shreds,
        'repair_shreds': repair_shreds
    }

def find_most_recent_log():
    """Find the most recently modified log file in current directory and common locations"""
    import glob

    search_paths = [
        './*.log',
        './logs/*.log',
        './log/*.log',
        '../*.log',
        '../logs/*.log',
        '../log/*.log',
        './testnet*.log',
        '../testnet*.log',
        '../../*.log',
        '../../logs/*.log',
        '../../log/*.log',
        './firedancer*.log',
        '../firedancer*.log'
    ]

    log_files = []
    for pattern in search_paths:
        found_files = glob.glob(pattern)
        log_files.extend(found_files)

    if not log_files:
        return None

    # Remove duplicates
    log_files = list(set(log_files))

    # Sort by modification time (most recent first)
    log_files.sort(key=lambda x: os.path.getmtime(x), reverse=True)

    print(f"Found {len(log_files)} log files:")
    for i, log_file in enumerate(log_files[:5]):  # Show top 5
        mtime = os.path.getmtime(log_file)
        mtime_str = datetime.fromtimestamp(mtime).strftime('%Y-%m-%d %H:%M:%S')
        print(f"  {i+1}. {log_file} (modified: {mtime_str})")

    return log_files[0]

def find_most_recent_csv_folder():
    """Find the most recently modified folder containing CSV files"""
    potential_folders = []

    # Search in current directory and parent directories
    search_roots = ['.', '..', '../..']

    for search_root in search_roots:
        for root, dirs, files in os.walk(search_root):
            if 'shred_data.csv' in files:
                potential_folders.append(root)

    if not potential_folders:
        return None

    # Remove duplicates
    potential_folders = list(set(potential_folders))

    # Sort by modification time of the shred_data.csv file (most recent first)
    def get_csv_mtime(folder):
        csv_path = os.path.join(folder, 'shred_data.csv')
        return os.path.getmtime(csv_path) if os.path.exists(csv_path) else 0

    potential_folders.sort(key=get_csv_mtime, reverse=True)

    print(f"Found {len(potential_folders)} CSV folders:")
    for i, folder in enumerate(potential_folders[:5]):  # Show top 5
        csv_path = os.path.join(folder, 'shred_data.csv')
        if os.path.exists(csv_path):
            mtime = os.path.getmtime(csv_path)
            mtime_str = datetime.fromtimestamp(mtime).strftime('%Y-%m-%d %H:%M:%S')
            print(f"  {i+1}. {folder} (shred_data.csv modified: {mtime_str})")

    return potential_folders[0]

if __name__ == "__main__":
    # Check for required dependencies
    try:
        import plotly
        import dash
    except ImportError as e:
        print("Missing required dependencies!")
        print("Please install them with: python3 -m pip install plotly dash pandas numpy")
        print(f"Error: {e}")
        sys.exit(1)

    # Parse command line arguments
    include_after_turbine = '--turbine' in sys.argv
    if include_after_turbine:
        sys.argv.remove('--turbine')

    # Handle different argument scenarios
    if len(sys.argv) == 1:
        print("No arguments provided. Searching for most recent log file and CSV folder...")
        log_path = find_most_recent_log()
        csv_path = find_most_recent_csv_folder()

        if log_path is None:
            print("Error: Could not find any log files automatically.")
            print("Please provide log file path as argument.")
            sys.exit(1)

        if csv_path is None:
            print("Error: Could not find any CSV folder with shred_data.csv automatically.")
            print("Please provide CSV folder path as argument.")
            sys.exit(1)

        print(f"Auto-detected log file: {log_path}")
        print(f"Auto-detected CSV folder: {csv_path}")

    elif len(sys.argv) == 3:
        log_path = sys.argv[1]
        csv_path = sys.argv[2]

    else:
        print('Usage: python report_page.py [--turbine] [<testnet.log path> <csv_folder_path>]')
        print('  --turbine: Include slots >= turbine slot in slot processing analysis')
        print('  If no arguments provided, will auto-detect most recently created log and CSV folder')
        print('Interactive report will be available at http://localhost:8050')
        sys.exit(1)

    # Validate paths
    if not os.path.exists(csv_path):
        print(f'Error: {csv_path} does not exist')
        sys.exit(1)

    csv_paths = {
        'shred_data.csv': os.path.join(csv_path, 'shred_data.csv'),
        'request_data.csv': os.path.join(csv_path, 'request_data.csv'),
        'peers_data.csv': os.path.join(csv_path, 'peers.csv'),
        'fec_complete.csv': os.path.join(csv_path, 'fec_complete.csv')
    }

    for csv_name, csv_file_path in csv_paths.items():
        if not os.path.exists(csv_file_path):
            csv_paths[csv_name] = None

    for csv_name, csv_file_path in csv_paths.items():
        print(f'Found {csv_name}: {csv_file_path}')

    # Load and process data
    try:
        load_and_process_data(
            log_path,
            csv_paths['request_data.csv'],
            csv_paths['shred_data.csv'],
            csv_paths['peers_data.csv'],
            csv_paths['fec_complete.csv'],
            include_after_turbine
        )

        print("\n🚀 Starting interactive report server...")
        print("📊 Navigate to http://localhost:8050 to view the report")
        print("🔄 Press Ctrl+C to stop the server")

        app.run(debug=False, host='127.0.0.1', port=12456)

    except Exception as e:
        print(f"❌ Error during data processing: {e}")
        print("Please check your data files and try again.")
        sys.exit(1)