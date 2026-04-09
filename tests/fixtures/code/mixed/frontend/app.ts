interface ApiClient {
    fetchUsers(): Promise<string[]>;
}

class DefaultClient implements ApiClient {
    constructor(private readonly baseUrl: string) {}

    async fetchUsers(): Promise<string[]> {
        return ["Ada", "Linus"];
    }
}

export function mount(): void {
    const client = new DefaultClient("http://localhost:8080");
    client.fetchUsers().then((users) => {
        for (const u of users) {
            console.log(u);
        }
    });
}
