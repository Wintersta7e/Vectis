public class Processor {
    private final String queueName;

    public Processor(String queueName) {
        this.queueName = queueName;
    }

    public void run() {
        System.out.println("processing " + queueName);
    }

    public String getQueueName() {
        return queueName;
    }
}
