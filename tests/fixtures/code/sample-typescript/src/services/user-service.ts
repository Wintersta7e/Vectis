import { User } from "../types";

export class UserService {
    private cache = new Map<string, User>();

    async findById(id: string): Promise<User | null> {
        const cached = this.cache.get(id);
        if (cached) {
            return cached;
        }
        return null;
    }

    async create(user: User): Promise<User> {
        this.cache.set(user.id, user);
        return user;
    }
}
