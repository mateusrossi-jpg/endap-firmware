export type UserRole = 'user' | 'operator' | 'integrator' | 'admin';

export interface StorageAdapter {
    getItem(key: string): string | null | Promise<string | null>;
    setItem(key: string, value: string): void | Promise<void>;
    removeItem?(key: string): void | Promise<void>;
}

export interface NodePoint {
    id: number;
    name: string;
    type: 'input' | 'output';
    value?: number;
}

export interface NodeItem {
    id: number;
    ip: string;
    profile: string;
    profile_label?: string;
    health: number;
    operational: boolean;
    last_transport: string;
    is_local?: boolean;
    io_points?: NodePoint[];
}

export class EndapApiService {
    private currentRole: UserRole = 'user';
    private storage: StorageAdapter | null = null;
    private baseUrl: string = '';

    constructor(storage?: StorageAdapter, baseUrl: string = '') {
        this.storage = storage || (typeof window !== 'undefined' ? window.localStorage : null);
        this.baseUrl = baseUrl;
    }

    public async init(): Promise<void> {
        try {
            if (this.storage) {
                const savedRole = await this.storage.getItem('endap_user_role');
                if (savedRole && ['user', 'operator', 'integrator', 'admin'].includes(savedRole)) {
                    this.currentRole = savedRole as UserRole;
                    return;
                }
            }
        } catch (e) {
            console.warn('[EndapApi] Storage role loading failed, falling back to default role: user', e);
        }
        this.currentRole = 'user';
    }

    public getRole(): UserRole {
        return this.currentRole;
    }

    public async setRole(role: UserRole): Promise<void> {
        this.currentRole = role;
        if (this.storage) {
            try {
                await this.storage.setItem('endap_user_role', role);
            } catch (e) {
                console.warn('[EndapApi] Failed to save role to storage:', e);
            }
        }
    }

    public async unlockRoleWithPin(pin: string): Promise<boolean> {
        // Standard PINs: 1234 -> operator, 8888 -> integrator, 9999 -> admin
        if (pin === '8888' || pin === '0000') {
            await this.setRole('integrator');
            return true;
        } else if (pin === '9999') {
            await this.setRole('admin');
            return true;
        } else if (pin === '1234') {
            await this.setRole('operator');
            return true;
        }
        return false;
    }

    public isAdvancedMode(): boolean {
        return this.currentRole === 'integrator' || this.currentRole === 'admin';
    }

    public async getNodes(): Promise<NodeItem[]> {
        const response = await fetch(`${this.baseUrl}/api/nodes`);
        if (!response.ok) {
            throw new Error(`Failed to fetch nodes: ${response.status}`);
        }
        const data = await response.json();
        return Array.isArray(data.nodes) ? data.nodes : [];
    }

    public async actuateResource(nodeId: number, ioId: number, value: number): Promise<boolean> {
        const response = await fetch(`${this.baseUrl}/api/resource/actuate`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                node_id: nodeId,
                io_id: ioId,
                value: value,
            }),
        });

        return response.ok || response.status === 202;
    }
}

export const endapApi = new EndapApiService();
