import { bind } from 'decko';
import { Component, h } from 'preact';

import { Terminal } from '../terminal';

import type { ITerminalOptions } from '@xterm/xterm';
import type { ClientOptions, FlowControl } from '../terminal/xterm';

interface Props {
    clientOptions: ClientOptions;
    termOptions: ITerminalOptions;
    flowControl: FlowControl;
}

interface TabInfo {
    id: number;
    title: string;
}

interface State {
    tabs: TabInfo[];
    activeId: number | null;
}

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const basePath = window.location.pathname.replace(/[/]+$/, '');
const apiTabsUrl = [window.location.protocol, '//', window.location.host, basePath, '/api/tabs'].join('');
const tokenUrl = [window.location.protocol, '//', window.location.host, basePath, '/token'].join('');
const REFRESH_INTERVAL = 5000;

export class Tabs extends Component<Props, State> {
    private refreshTimer: ReturnType<typeof setInterval> | null = null;
    private terminalRefs: Record<number, Terminal> = {};

    state: State = {
        tabs: [],
        activeId: null,
    };

    componentDidMount() {
        this.refresh().then(tabs =>
            this.setState(prev => ({
                tabs,
                activeId: prev.activeId ?? (tabs.length > 0 ? tabs[0].id : null),
            }))
        );
        // keep titles in sync and drop tabs whose sessions were removed server-side
        this.refreshTimer = setInterval(() => this.refresh().then(tabs => this.syncTabs(tabs)), REFRESH_INTERVAL);
    }

    componentWillUnmount() {
        if (this.refreshTimer !== null) {
            clearInterval(this.refreshTimer);
        }
    }

    private async refresh(): Promise<TabInfo[]> {
        try {
            const resp = await fetch(apiTabsUrl);
            if (!resp.ok) return this.state.tabs;
            const data = (await resp.json()) as { id: number; title: string }[];
            return data;
        } catch (e) {
            console.error('[ttyd] refresh tabs: ', e);
            return this.state.tabs;
        }
    }

    private syncTabs(tabs: TabInfo[]) {
        const activeId =
            this.state.activeId !== null && !tabs.some(t => t.id === this.state.activeId)
                ? tabs.length > 0
                    ? tabs[0].id
                    : null
                : this.state.activeId;
        this.setState({ tabs, activeId });
    }

    @bind
    private async addTab() {
        try {
            const resp = await fetch(apiTabsUrl, { method: 'POST' });
            if (!resp.ok) {
                console.error('[ttyd] create tab failed: ', resp.status);
                return;
            }
            const { id } = (await resp.json()) as { id: number };
            const tabs = await this.refresh();
            this.setState({ tabs, activeId: id });
        } catch (e) {
            console.error('[ttyd] create tab: ', e);
        }
    }

    @bind
    private async removeTab(id: number) {
        try {
            await fetch(`${apiTabsUrl}/${id}`, { method: 'DELETE' });
        } catch (e) {
            console.error('[ttyd] remove tab: ', e);
        }
        const tabs = (await this.refresh()).filter(t => t.id !== id);
        const activeId = this.state.activeId === id ? (tabs.length > 0 ? tabs[0].id : null) : this.state.activeId;
        this.setState({ tabs, activeId });
    }

    @bind
    private activateTab(id: number) {
        if (this.state.activeId === id) return;
        this.setState({ activeId: id });
        requestAnimationFrame(() => this.terminalRefs[id]?.fit());
    }

    render(_props: Props, { tabs, activeId }: State) {
        const { clientOptions, termOptions, flowControl } = this.props;
        return (
            <div class="tabbed-app">
                <div class="tab-bar">
                    {tabs.map(t => (
                        <div
                            key={t.id}
                            class={'tab' + (t.id === activeId ? ' active' : '')}
                            title={t.title}
                            onClick={() => this.activateTab(t.id)}
                        >
                            <span class="tab-title">{t.title || 'terminal'}</span>
                            <span
                                class="tab-close"
                                title="Close tab (kills this tmux session)"
                                onClick={e => {
                                    e.stopPropagation();
                                    this.removeTab(t.id);
                                }}
                            >
                                &times;
                            </span>
                        </div>
                    ))}
                    <div class="tab-add" title="New tab" onClick={this.addTab}>
                        +
                    </div>
                </div>
                <div class="tab-content">
                    {tabs.length === 0 && (
                        <div class="tab-empty" onClick={this.addTab}>
                            <div class="tab-empty-add">+</div>
                            <div>New terminal tab</div>
                        </div>
                    )}
                    {tabs.map(t => (
                        <div key={t.id} class="tab-pane" style={{ display: t.id === activeId ? 'block' : 'none' }}>
                            <Terminal
                                id={'terminal-' + t.id}
                                wsUrl={[protocol, '//', window.location.host, basePath, '/ws?tab=', t.id].join('')}
                                tokenUrl={tokenUrl}
                                mousePaste
                                clientOptions={clientOptions}
                                termOptions={termOptions}
                                flowControl={flowControl}
                                ref={el => {
                                    if (el) this.terminalRefs[t.id] = el;
                                }}
                            />
                        </div>
                    ))}
                </div>
            </div>
        );
    }
}
