class Counter {
    constructor(initial = 0) {
        this.value = initial;
    }

    increment() {
        this.value += 1;
        return this.value;
    }

    reset() {
        this.value = 0;
    }
}

function createCounter(initial) {
    return new Counter(initial);
}

function main() {
    const counter = createCounter(10);
    counter.increment();
    console.log(counter.value);
}

main();
