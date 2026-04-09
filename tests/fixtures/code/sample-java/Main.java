public class Main {
    public static void main(String[] args) {
        User user = new User("u-1", "Ada");
        System.out.println("hello, " + user.getName());
    }

    public static String version() {
        return "0.1.0";
    }
}
