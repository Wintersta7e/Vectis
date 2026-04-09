import { UserService } from "./services/user-service";
import { User } from "./types";

async function main(): Promise<void> {
    const service = new UserService();
    const user: User | null = await service.findById("u-1");
    if (user) {
        console.log(`hello, ${user.name}`);
    }
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});
