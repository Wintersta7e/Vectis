export interface User {
    id: string;
    name: string;
    email?: string;
}

export interface Repository<T> {
    findById(id: string): Promise<T | null>;
    save(item: T): Promise<T>;
}

export type UserId = string;

export type Result<T> = { ok: true; value: T } | { ok: false; error: string };
