module Greetings
  class Greeter
    def initialize(name)
      @name = name
    end

    def hello
      "hello, #{@name}"
    end

    def shout
      hello.upcase + '!'
    end

    def self.default
      new('world')
    end
  end

  class AnonymousGreeter < Greeter
    def initialize
      super('stranger')
    end
  end
end
